#include "KeysModule.h"

#include "KeysTab.h"
#include "KeysView.h"
#include "modules/hwi/HwiModule.h"
#include "modules/keys/flow/SigningFlow.h"
#include "modules/keys/signers/OfflineSigner.h"
#include "modules/keys/signers/RouteSigner.h"

#include "AppController.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "qr/QrTypes.h"

#include <QDebug>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <utility>

#include <common.h>

namespace keys {

namespace {

auto failedOp(const QString &error) -> KeyOpResult {
    KeyOpResult result;
    result.is_ok = false;
    result.error = rust::String(error.toStdString());
    return result;
}

// One derivation path written two ways: with or without the leading "m/", hardened
// steps as ' or h. The cache writes "84'/1'/0'", callers ask "m/84'/1'/0'".
auto samePath(QString lhs, QString rhs) -> bool {
    auto normalize = [](QString &path) {
        path.replace(QChar('h'), QChar('\''));
        if (path.startsWith(QStringLiteral("m/"))) {
            path.remove(0, 2);
        }
    };
    normalize(lhs);
    normalize(rhs);
    return lhs == rhs;
}

} // namespace

Network KeysModule::s_network = Network::Bitcoin;

KeysBus::KeysBus(KeysModule *module, QObject *parent) : IKeys(parent), m_module(module) {
}

QList<KeyEntry> KeysBus::keys(Network network) const {
    QList<KeyEntry> entries;
    NetworkScope scope(m_module, network);
    for (const auto &info : m_module->keys()) {
        entries.append(m_module->buildKeyEntry(info));
    }
    return entries;
}

auto KeysBus::newRequest() -> ReqId {
    return m_module->nextReqId();
}

void KeysBus::sign(ReqId req_id, Network network, const QString &psbt_v2_hex,
                   const QStringList &descriptors, const QString &wallet_name) {
    NetworkScope scope(m_module, network);
    m_module->sign(req_id, psbt_v2_hex, descriptors, wallet_name);
}

void KeysBus::verifyAddress(ReqId req_id, Network network, const QString &descriptor,
                            const QString &path, const QString &address) {
    NetworkScope scope(m_module, network);
    m_module->verifyAddress(req_id, descriptor, path, address);
}

void KeysBus::spDescriptor(ReqId req_id, Network network, const QString &fingerprint) {
    NetworkScope scope(m_module, network);
    m_module->spDescriptor(req_id, fingerprint);
}

void KeysBus::xpub(ReqId req_id, Network network, const QString &fingerprint, const QString &path) {
    NetworkScope scope(m_module, network);
    m_module->fetchXpub(req_id, fingerprint, path);
}

auto KeysBus::cachedXpub(Network network, const QString &fingerprint, const QString &path) const
    -> QString {
    NetworkScope scope(m_module, network);
    return m_module->cachedXpub(fingerprint, path);
}

auto KeysBus::registrationHmac(Network network, const QString &fingerprint,
                               const QString &descriptor) const -> QString {
    NetworkScope scope(m_module, network);
    return m_module->registrationHmac(fingerprint, descriptor);
}

auto KeysBus::storeRegistrationHmac(Network network, const QString &fingerprint,
                                    const QString &descriptor, const QString &hmac) -> bool {
    NetworkScope scope(m_module, network);
    return m_module->storeRegistrationHmac(fingerprint, descriptor, hmac);
}

void KeysBus::registerDescriptor(ReqId req_id, Network network, const QString &fingerprint,
                                 const QString &descriptor) {
    NetworkScope scope(m_module, network);
    m_module->registerDescriptor(req_id, fingerprint, descriptor);
}

void KeysBus::deliverSigned(ReqId req_id, const QString &psbt_v2_hex) {
    emit signed_(req_id, psbt_v2_hex);
}

void KeysBus::deliverVerified(ReqId req_id, bool matched) {
    emit verified(req_id, matched);
}

void KeysBus::deliverError(std::optional<ReqId> req_id, const QString &message,
                           plugin::SignError kind) {
    emit error(req_id, message, kind);
}

void KeysBus::deliverCancelled(ReqId req_id) {
    emit cancelled(req_id);
}

void KeysBus::deliverSpDescriptor(ReqId req_id, const QString &descriptor,
                                  const QString &fingerprint) {
    emit spDescriptorReady(req_id, descriptor, fingerprint);
}

void KeysBus::deliverXpub(ReqId req_id, const QString &xpub, const QString &fingerprint,
                          const QString &path) {
    emit xpubReady(req_id, xpub, fingerprint, path);
}

void KeysBus::deliverDescriptorRegistered(ReqId req_id, const QString &fingerprint) {
    emit descriptorRegistered(req_id, fingerprint);
}

KeysModule::KeysModule(QObject *parent) : IModule(parent) {
    connect(Host::get()->events(), &HostEvents::instanceRegistered, this,
            &KeysModule::onInstanceRegistered, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &KeysModule::onInstanceRemoved, qontrol::UNIQUE);
    connect(this, &KeysModule::mnemonicGenerationFinished, this,
            &KeysModule::onMnemonicGenerationFinished, qontrol::UNIQUE);
    connect(this, &KeysModule::hotKeySealed, this, &KeysModule::onHotKeySealed, qontrol::UNIQUE);
    connect(this, &KeysModule::mnemonicRevealFinished, this, &KeysModule::onMnemonicRevealFinished,
            qontrol::UNIQUE);
    connect(this, &KeysModule::hotSignFinished, this, &KeysModule::onHotSignFinished,
            qontrol::UNIQUE);
    connect(this, &KeysModule::hotSpDescriptorFinished, this,
            &KeysModule::onHotSpDescriptorFinished, qontrol::UNIQUE);
    connect(this, &KeysModule::hotXpubFinished, this, &KeysModule::onHotXpubFinished,
            qontrol::UNIQUE);
    createModule();
    m_bus = std::make_unique<KeysBus>(this);
    Host::get()->setKeys(m_bus.get(), this);
    connect(this, &KeysModule::keysChanged, this, &KeysModule::updateSignerPolling,
            qontrol::UNIQUE);
    bindHwiModule();
    refreshLiveDevices();
    updateSignerPolling();
}

KeysModule::~KeysModule() {
    teardownModule();
}

auto KeysModule::toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

QMap<QString, QString> KeysModule::meta() const {
    return {{QStringLiteral("id"), QStringLiteral("keys")},
            {QStringLiteral("name"), QStringLiteral("Keys")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

QList<IModule *> KeysModule::modules() const {
    // IPlugin::modules() is const, but the host activates the module it gets back.
    return {const_cast<KeysModule *>(this)};
}

ReqId KeysModule::list() {
    // Nothing to launch from the launcher: this module's widget is hosted directly by
    // the main window, not through an instance.
    ReqId reqId = nextReqId();
    emit IModule::instances(reqId, QList<QPair<QString, QString>>{});
    return reqId;
}

ReqId KeysModule::autoStart() {
    // This module pushes no instance: its widget is hosted directly by the main
    // window in a later task.
    ReqId reqId = nextReqId();
    emit IModule::autoStart(reqId, QStringList{});
    return reqId;
}

void KeysModule::startInstance(const QString &id) {
    emit error(std::nullopt, QStringLiteral("keys: unknown instance %1").arg(id));
}

void KeysModule::createModule() {
    s_network = ::hwi_config_network();
    static_cast<void>(handle(s_network));
}

void KeysModule::teardownModule() {
    m_handles.clear();
}

Network KeysModule::network() {
    return s_network;
}

void KeysModule::setNetwork(Network network) {
    if (s_network == network) {
        return;
    }
    // Does not touch hwi_set_config_network: the tab calls HwiModule::setNetwork for
    // that, which owns the persistence.
    auto *opened = handle(network);
    if (!opened->is_ok()) {
        qWarning() << "keys:" << toQt(opened->get_error());
        return;
    }
    s_network = network;
    emit keysChanged();
}

void KeysModule::setTabShown(bool shown) {
    m_tab_shown = shown;
    updateSignerPolling();
}

void KeysModule::updateSignerPolling() {
    bool usbKey = std::ranges::any_of(tabKeys(), [](const KeyInfo &info) {
        return std::ranges::find(info.routes, KeyRouteKind::Usb) != info.routes.end();
    });
    Host::get()->requestSignerPolling(this, m_tab_shown || usbKey);
}

QList<KeyInfo> KeysModule::keys() const {
    QList<KeyInfo> keys;
    if (rust() == nullptr) {
        return keys;
    }
    for (const auto &key : rust()->keys()) {
        keys.append(key);
    }
    return keys;
}

QList<KeyInfo> KeysModule::tabKeys() const {
    QList<KeyInfo> keys;
    for (const auto &key : handle(s_network)->keys()) {
        keys.append(key);
    }
    return keys;
}

bool KeysModule::hasKey(const QString &fingerprint) const {
    auto matches = keys();
    return std::ranges::any_of(matches, [&fingerprint](const KeyInfo &key) {
        return QString::compare(toQt(key.fingerprint), fingerprint, Qt::CaseInsensitive) == 0;
    });
}

auto KeysModule::countRegisteredDescriptors(const QString &fingerprint) const -> int {
    if (rust() == nullptr) {
        return 0;
    }
    int count = 0;
    for (const auto &entry : rust()->key_descriptors(rust::String(fingerprint.toStdString()))) {
        if (entry.registered) {
            ++count;
        }
    }
    return count;
}

auto KeysModule::buildKeyEntry(const KeyInfo &info) -> KeyEntry {
    QString fingerprint = toQt(info.fingerprint);
    KeyEntry entry;
    entry.fingerprint = fingerprint;
    entry.alias = toQt(info.alias);
    entry.descriptor_route = descriptorRoute(info);
    entry.can_show_address = std::ranges::any_of(info.routes, [](KeyRouteKind route) {
        return route == KeyRouteKind::Usb || route == KeyRouteKind::Qr;
    });

    for (const auto &route : info.routes) {
        switch (route) {
        case KeyRouteKind::Usb: {
            auto *deviceSigner = findLiveDeviceSigner(fingerprint);
            if (deviceSigner != nullptr) {
                entry.can_sign = true;
                entry.signer = deviceSigner;
                return entry;
            }
            break;
        }
        case KeyRouteKind::Hot:
            if (info.has_hot_secret) {
                entry.can_sign = true;
                entry.signer = routeSignerFor(fingerprint, info, KeyRouteKind::Hot);
                return entry;
            }
            break;
        case KeyRouteKind::Qr:
        case KeyRouteKind::SdCard:
            entry.can_sign = true;
            entry.signer = routeSignerFor(fingerprint, info, route);
            return entry;
        case KeyRouteKind::Unknown:
        default:
            break;
        }
    }

    entry.can_sign = false;
    entry.signer = offlineSignerFor(fingerprint, info);
    return entry;
}

auto KeysModule::descriptorRoute(const KeyInfo &info) -> KeyRouteKind {
    for (const auto &route : info.routes) {
        if (route == KeyRouteKind::Qr || (route == KeyRouteKind::Hot && info.has_hot_secret)) {
            return route;
        }
    }
    return KeyRouteKind::Unknown;
}

auto KeysModule::xpubRoute(const KeyInfo &info) -> KeyRouteKind {
    if (findLiveDeviceSigner(toQt(info.fingerprint)) != nullptr) {
        return KeyRouteKind::Usb;
    }
    return descriptorRoute(info);
}

auto KeysModule::findLiveDeviceSigner(const QString &fingerprint) -> ISigner * {
    for (const auto &live : Host::get()->signers()) {
        if (live.signer != nullptr && live.signer->fingerprint() == fingerprint) {
            return live.signer;
        }
    }
    return nullptr;
}

auto KeysModule::routeSignerFor(const QString &fingerprint, const KeyInfo &info, KeyRouteKind route)
    -> ISigner * {
    QString cacheKey = signerKey(fingerprint);
    auto *cached = m_signers.value(cacheKey, nullptr);
    auto *cachedRoute = qobject_cast<RouteSigner *>(cached);
    if (cachedRoute != nullptr && cachedRoute->route() == route) {
        return cachedRoute;
    }
    auto *created = new RouteSigner(this, info, route, this);
    if (cached != nullptr) {
        cached->deleteLater();
    }
    m_signers.insert(cacheKey, created);
    return created;
}

auto KeysModule::signerKey(const QString &fingerprint) const -> QString {
    return QStringLiteral("%1/%2").arg(static_cast<int>(activeNetwork())).arg(fingerprint);
}

auto KeysModule::offlineSignerFor(const QString &fingerprint, const KeyInfo &info) -> ISigner * {
    QString cacheKey = signerKey(fingerprint);
    auto *cached = m_signers.value(cacheKey, nullptr);
    if (qobject_cast<OfflineSigner *>(cached) != nullptr) {
        return cached;
    }
    auto *created = new OfflineSigner(this, info, this);
    if (cached != nullptr) {
        cached->deleteLater();
    }
    m_signers.insert(cacheKey, created);
    return created;
}

void KeysModule::runFlow(flow::SigningFlow *flow) {
    connect(flow, &flow::SigningFlow::psbtSigned, m_bus.get(), &KeysBus::deliverSigned,
            qontrol::UNIQUE);
    connect(flow, &flow::SigningFlow::addressVerified, m_bus.get(), &KeysBus::deliverVerified,
            qontrol::UNIQUE);
    connect(flow, &flow::SigningFlow::descriptorFetched, this, &KeysModule::onFlowDescriptorFetched,
            qontrol::UNIQUE);
    connect(flow, &flow::SigningFlow::xpubFetched, this, &KeysModule::onFlowXpubFetched,
            qontrol::UNIQUE);
    connect(flow, &flow::SigningFlow::failed, m_bus.get(), &KeysBus::deliverError, qontrol::UNIQUE);
    connect(flow, &flow::SigningFlow::cancelled, m_bus.get(), &KeysBus::deliverCancelled,
            qontrol::UNIQUE);
    flow->begin();
    if (flow->answered()) {
        // A route that failed at once has answered already: there is nothing to show.
        delete flow;
        return;
    }
    AppController::execModal(flow);
}

void KeysModule::sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors,
                      const QString &wallet_name) {
    if (rust() == nullptr) {
        m_bus->deliverError(req_id, TR("keys-module-not-ready"));
        return;
    }
    // signing_plan() blocks on nothing (no device I/O), so calling it from the GUI
    // thread is safe: see its doc comment in lib/include/silent.h.
    auto plan = rust()->signing_plan(rust::String(psbt_v2_hex.toStdString()));
    if (!plan.is_ok) {
        m_bus->deliverError(req_id, toQt(plan.error));
        return;
    }
    runFlow(new flow::SigningFlow(
        this, {.context = flow::Context::Sign,
               .req_id = req_id,
               .psbt_v2_hex = psbt_v2_hex,
               .descriptor = descriptors.isEmpty() ? QString() : descriptors.first(),
               .wallet_name = wallet_name}));
}

void KeysModule::verifyAddress(ReqId req_id, const QString &descriptor, const QString &path,
                               const QString &address) {
    if (rust() == nullptr) {
        m_bus->deliverError(req_id, TR("keys-module-not-ready"));
        return;
    }
    runFlow(new flow::SigningFlow(this, {.context = flow::Context::Verify,
                                         .req_id = req_id,
                                         .descriptor = descriptor,
                                         .path = path,
                                         .address = address}));
}

void KeysModule::spDescriptor(ReqId req_id, const QString &fingerprint) {
    auto infos = keys();
    auto found = std::ranges::find_if(infos, [&fingerprint](const KeyInfo &info) {
        return QString::compare(toQt(info.fingerprint), fingerprint, Qt::CaseInsensitive) == 0;
    });
    if (found == infos.end() || descriptorRoute(*found) == KeyRouteKind::Unknown) {
        m_bus->deliverError(req_id, TR("keys-sp-descriptor-unavailable"));
        return;
    }
    runFlow(new flow::SigningFlow(
        this, {.context = flow::Context::Fetch, .req_id = req_id, .fingerprint = fingerprint}));
}

void KeysModule::fetchXpub(ReqId req_id, const QString &fingerprint, const QString &path) {
    if (rust() == nullptr) {
        m_bus->deliverError(req_id, TR("keys-module-not-ready"));
        return;
    }
    if (auto cached = cachedXpub(fingerprint, path); !cached.isEmpty()) {
        m_bus->deliverXpub(req_id, cached, fingerprint, path);
        return;
    }
    auto infos = keys();
    auto found = std::ranges::find_if(infos, [&fingerprint](const KeyInfo &info) {
        return QString::compare(toQt(info.fingerprint), fingerprint, Qt::CaseInsensitive) == 0;
    });
    if (found == infos.end() || xpubRoute(*found) == KeyRouteKind::Unknown) {
        m_bus->deliverError(req_id, TR("keys-xpub-unavailable"));
        return;
    }
    runFlow(new flow::SigningFlow(this, {.context = flow::Context::FetchXpub,
                                         .req_id = req_id,
                                         .path = path,
                                         .fingerprint = fingerprint}));
}

auto KeysModule::cachedXpub(const QString &fingerprint, const QString &path) const -> QString {
    if (rust() == nullptr) {
        return {};
    }
    for (const auto &cached : rust()->key_xpubs(rust::String(fingerprint.toStdString()))) {
        if (samePath(toQt(cached.path), path)) {
            return toQt(cached.xpub);
        }
    }
    return {};
}

auto KeysModule::registrationHmac(const QString &fingerprint, const QString &descriptor) const
    -> QString {
    if (rust() == nullptr) {
        return {};
    }
    for (const auto &entry : rust()->key_descriptors(rust::String(fingerprint.toStdString()))) {
        if (toQt(entry.descriptor) == descriptor) {
            return toQt(entry.hmac);
        }
    }
    return {};
}

auto KeysModule::storeRegistrationHmac(const QString &fingerprint, const QString &descriptor,
                                       const QString &hmac) -> bool {
    if (rust() == nullptr) {
        qWarning() << "keys: registration proof not stored, module not ready";
        return false;
    }
    auto result = rust()->set_descriptor_hmac(rust::String(fingerprint.toStdString()),
                                              rust::String(descriptor.toStdString()),
                                              rust::String(hmac.toStdString()));
    if (!result.is_ok) {
        qWarning() << "keys: registration proof not stored:" << toQt(result.error);
        return false;
    }
    emit keysChanged();
    return true;
}

void KeysModule::registerDescriptor(ReqId req_id, const QString &fingerprint,
                                    const QString &descriptor) {
    if (rust() == nullptr) {
        m_bus->deliverError(req_id, TR("keys-module-not-ready"));
        return;
    }
    auto result = rust()->register_descriptor(rust::String(fingerprint.toStdString()),
                                              rust::String(descriptor.toStdString()));
    if (!result.is_ok) {
        m_bus->deliverError(req_id, toQt(result.error));
        return;
    }
    emit keysChanged();
    auto device = registrationDevice(fingerprint);
    if (!device.has_value()) {
        m_bus->deliverDescriptorRegistered(req_id, fingerprint);
        return;
    }
    connect(device->signer, &ISigner::descriptorIsRegistered, this,
            &KeysModule::onDeviceIsRegistered, qontrol::UNIQUE);
    connect(device->signer, &ISigner::descriptorRegistered, this, &KeysModule::onDeviceRegistered,
            qontrol::UNIQUE);
    connect(device->signer, &ISigner::error, this, &KeysModule::onDeviceRegisterError,
            qontrol::UNIQUE);
    sendDeviceRegistration({.bus_req = req_id,
                            .fingerprint = fingerprint,
                            .descriptor = descriptor,
                            .instance_id = device->instanceId,
                            .signer = device->signer});
}

auto KeysModule::registrationDevice(const QString &fingerprint) const
    -> std::optional<plugin::SignerEntry> {
    auto infos = keys();
    auto found = std::ranges::find_if(infos, [&fingerprint](const KeyInfo &info) {
        return QString::compare(toQt(info.fingerprint), fingerprint, Qt::CaseInsensitive) == 0;
    });
    if (found == infos.end() ||
        std::ranges::find(found->routes, KeyRouteKind::Usb) == found->routes.end()) {
        return std::nullopt;
    }
    return readyDevice(fingerprint);
}

void KeysModule::sendDeviceRegistration(const DeviceRegistration &registration) {
    ReqId deviceReq = registration.checking
                          ? registration.signer->isDescriptorRegistered(registration.descriptor)
                          : registration.signer->registerDescriptor(registration.descriptor);
    m_device_registrations.insert(deviceReq, registration);
}

void KeysModule::finishDeviceRegistration(const DeviceRegistration &registration,
                                          const std::optional<QString> &error) {
    if (error.has_value()) {
        m_bus->deliverError(registration.bus_req, error.value());
    } else {
        m_bus->deliverDescriptorRegistered(registration.bus_req, registration.fingerprint);
    }
    ISigner *signer = registration.signer.data();
    if (signer == nullptr) {
        return;
    }
    for (const auto &pending : std::as_const(m_device_registrations)) {
        if (pending.signer == signer) {
            return;
        }
    }
    disconnect(signer, &ISigner::descriptorIsRegistered, this, &KeysModule::onDeviceIsRegistered);
    disconnect(signer, &ISigner::descriptorRegistered, this, &KeysModule::onDeviceRegistered);
    disconnect(signer, &ISigner::error, this, &KeysModule::onDeviceRegisterError);
}

void KeysModule::generateMnemonic(ReqId req_id, std::uint32_t word_count) {
    auto *thread = QThread::create([this, req_id, word_count]() {
        emit mnemonicGenerationFinished(req_id, ::generate_mnemonic(word_count));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onMnemonicGenerationFinished(ReqId req_id, const MnemonicResult &result) {
    emit mnemonicGenerated(req_id, result);
}

void KeysModule::importHotKey(ReqId req_id, const QString &mnemonic, const QString &alias,
                              const QString &password) {
    Network network = KeysModule::network();
    auto *thread = QThread::create([this, req_id, mnemonic = mnemonic.toStdString(),
                                    password = password.toStdString(), alias, network]() mutable {
        auto sealed = ::hot_seal(rust::String(mnemonic), rust::String(password), network);
        mnemonic.assign(mnemonic.size(), '\0');
        mnemonic.clear();
        password.assign(password.size(), '\0');
        password.clear();
        emit hotKeySealed(req_id, sealed.into_raw(), alias);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onHotKeySealed(ReqId req_id, SealedHotKey *sealed, const QString &alias) {
    auto box = rust::Box<SealedHotKey>::from_raw(sealed);
    KeyInfoResult result;
    if (!box->is_ok()) {
        result.is_ok = false;
        result.error = box->get_error();
        emit hotKeyImported(req_id, result);
        return;
    }
    if (rust() == nullptr) {
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        emit hotKeyImported(req_id, result);
        return;
    }
    result = rust()->commit_hot_key(*box, rust::String(alias.toStdString()));
    if (result.is_ok) {
        emit keysChanged();
    }
    emit hotKeyImported(req_id, result);
}

void KeysModule::revealMnemonic(ReqId req_id, const QString &fingerprint, const QString &password) {
    if (rust() == nullptr) {
        MnemonicResult result{};
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        emit mnemonicRevealFinished(req_id, result);
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        MnemonicResult result{};
        result.is_ok = false;
        result.error = material->get_error();
        emit mnemonicRevealFinished(req_id, result);
        return;
    }
    auto *thread = QThread::create([this, req_id, material = material.into_raw(),
                                    password = password.toStdString()]() mutable {
        auto box = rust::Box<HotMaterial>::from_raw(material);
        auto result = ::hot_reveal(*box, rust::String(password));
        password.assign(password.size(), '\0');
        password.clear();
        emit mnemonicRevealFinished(req_id, result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onMnemonicRevealFinished(ReqId req_id, const MnemonicResult &result) {
    emit mnemonicRevealed(req_id, result);
}

void KeysModule::signHot(ReqId req_id, const QString &fingerprint, const QString &password,
                         const QString &psbt_v2_hex) {
    if (rust() == nullptr) {
        emit hotSignFinished(req_id, false, QString(), TR("keys-module-not-ready"), false, false);
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        emit hotSignFinished(req_id, false, QString(), toQt(material->get_error()), false, false);
        return;
    }
    auto *thread = QThread::create([this, req_id, material = material.into_raw(),
                                    password = password.toStdString(),
                                    psbt = psbt_v2_hex.toStdString()]() mutable {
        auto box = rust::Box<HotMaterial>::from_raw(material);
        auto result = ::hot_sign(*box, password, psbt);
        password.assign(password.size(), '\0');
        password.clear();
        emit hotSignFinished(req_id, result.is_ok, toQt(result.psbt_v2_hex), toQt(result.error),
                             result.wrong_password, result.musig_session);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onHotSignFinished(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error,
                                   bool wrong_password, bool musig_session) {
    emit hotSigned(req_id, is_ok, std::move(psbt_v2_hex), std::move(error), wrong_password,
                   musig_session);
}

void KeysModule::fetchHotSpDescriptor(ReqId req_id, const QString &fingerprint,
                                      const QString &password) {
    if (rust() == nullptr) {
        emit hotSpDescriptorFinished(req_id, false, QString(), TR("keys-module-not-ready"), false);
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        emit hotSpDescriptorFinished(req_id, false, QString(), toQt(material->get_error()), false);
        return;
    }
    auto *thread = QThread::create([this, req_id, material = material.into_raw(),
                                    password = password.toStdString()]() mutable {
        auto box = rust::Box<HotMaterial>::from_raw(material);
        auto result = ::hot_sp_descriptor(*box, password);
        password.assign(password.size(), '\0');
        password.clear();
        emit hotSpDescriptorFinished(req_id, result.is_ok, toQt(result.descriptor),
                                     toQt(result.error), result.wrong_password);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onHotSpDescriptorFinished(ReqId req_id, bool is_ok, QString descriptor,
                                           QString error, bool wrong_password) {
    emit hotSpDescriptorFetched(req_id, is_ok, std::move(descriptor), std::move(error),
                                wrong_password);
}

void KeysModule::fetchHotXpub(ReqId req_id, const QString &fingerprint, const QString &password,
                              const QString &path) {
    if (rust() == nullptr) {
        emit hotXpubFinished(req_id, false, QString(), TR("keys-module-not-ready"), false);
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        emit hotXpubFinished(req_id, false, QString(), toQt(material->get_error()), false);
        return;
    }
    auto *thread =
        QThread::create([this, req_id, material = material.into_raw(),
                         password = password.toStdString(), path = path.toStdString()]() mutable {
            auto box = rust::Box<HotMaterial>::from_raw(material);
            auto result = ::hot_xpub(*box, password, path);
            password.assign(password.size(), '\0');
            password.clear();
            emit hotXpubFinished(req_id, result.is_ok, toQt(result.xpub), toQt(result.error),
                                 result.wrong_password);
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onHotXpubFinished(ReqId req_id, bool is_ok, QString xpub, QString error,
                                   bool wrong_password) {
    emit hotXpubFetched(req_id, is_ok, std::move(xpub), std::move(error), wrong_password);
}

void KeysModule::onFlowXpubFetched(ReqId req_id, const QString &xpub, const QString &fingerprint,
                                   const QString &path) {
    // Cached even when the caller is gone: the next ask needs no device.
    auto cached = cacheXpub(fingerprint, path, xpub);
    if (!cached.is_ok) {
        qWarning() << "keys:" << toQt(cached.error);
    }
    m_bus->deliverXpub(req_id, xpub, fingerprint, path);
}

void KeysModule::onFlowDescriptorFetched(ReqId req_id, const QString &descriptor,
                                         const QString &fingerprint,
                                         const QList<qr::QrXpub> &xpubs) {
    // Cached first: the caller reads the cache when the descriptor lands.
    for (const auto &xpub : xpubs) {
        auto cached = cacheXpub(fingerprint, xpub.path, xpub.xpub);
        if (!cached.is_ok) {
            qWarning() << "keys:" << toQt(cached.error);
        }
    }
    m_bus->deliverSpDescriptor(req_id, descriptor, fingerprint);
}

void KeysModule::saveSdPsbt(ReqId req_id, const QString &path, const QString &psbt_v2_hex) {
    auto *thread = QThread::create(
        [this, req_id, path = path.toStdString(), psbt = psbt_v2_hex.toStdString()]() {
            auto result = ::sd_save_psbt(rust::String(path), rust::String(psbt));
            emit sdSaved(req_id, result.is_ok, toQt(result.error));
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::loadSdPsbt(ReqId req_id, const QString &path) {
    auto *thread = QThread::create([this, req_id, path = path.toStdString()]() {
        auto result = ::sd_load_psbt(rust::String(path));
        emit sdLoaded(req_id, result.is_ok, toQt(result.psbt_hex), toQt(result.error));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

auto KeysModule::setAlias(const QString &fingerprint, const QString &alias) -> KeyOpResult {
    if (rust() == nullptr) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = rust()->set_alias(rust::String(fingerprint.toStdString()),
                                    rust::String(alias.toStdString()));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::addRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult {
    if (rust() == nullptr) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = rust()->add_route(rust::String(fingerprint.toStdString()), kind);
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::removeRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult {
    if (rust() == nullptr) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = rust()->remove_route(rust::String(fingerprint.toStdString()), kind);
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::setRouteOrder(const QString &fingerprint, rust::Vec<KeyRouteKind> order)
    -> KeyOpResult {
    if (rust() == nullptr) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result =
        rust()->set_route_order(rust::String(fingerprint.toStdString()), std::move(order));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::removeKey(const QString &fingerprint, bool force) -> KeyOpResult {
    if (rust() == nullptr) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = rust()->remove_key(rust::String(fingerprint.toStdString()), force);
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::addKey(const QString &fingerprint, const QString &alias,
                        const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs)
    -> KeyOpResult {
    auto result = insertKey(fingerprint, alias, routes, xpubs);
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

// Writes the key store through the handle: not const, whatever rust()'s constness says.
// NOLINTNEXTLINE(readability-make-member-function-const)
auto KeysModule::insertKey(const QString &fingerprint, const QString &alias,
                           const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs)
    -> KeyOpResult {
    if (rust() == nullptr) {
        return failedOp(TR("keys-module-not-ready"));
    }
    auto key = rust::String(fingerprint.toStdString());
    auto added = rust()->add_key(key, rust::String(alias.toStdString()));
    if (!added.is_ok) {
        return failedOp(toQt(added.error));
    }
    KeyOpResult result{.is_ok = true, .error = rust::String()};
    for (auto route : routes) {
        if (result.is_ok) {
            result = rust()->add_route(key, route);
        }
    }
    for (const auto &entry : xpubs) {
        if (result.is_ok) {
            result = rust()->cache_xpub(key, rust::String(entry.path.toStdString()),
                                        rust::String(entry.xpub.toStdString()));
        }
    }
    if (!result.is_ok) {
        // A key half added would show up without its routes or xpubs.
        auto removed = rust()->remove_key(key, true);
        if (!removed.is_ok) {
            qWarning() << "keys:" << toQt(removed.error);
        }
    }
    return result;
}

auto KeysModule::addDeviceKey(const plugin::SignerEntry &entry, const QString &alias,
                              const QList<PathXpub> &xpubs) -> KeyOpResult {
    QString fingerprint = entry.signer->fingerprint();
    auto result = insertKey(fingerprint, alias, {KeyRouteKind::Usb}, xpubs);
    if (!result.is_ok) {
        return result;
    }
    result = rust()->record_device(toLiveDevice(entry));
    if (!result.is_ok) {
        auto removed = rust()->remove_key(rust::String(fingerprint.toStdString()), true);
        if (!removed.is_ok) {
            qWarning() << "keys:" << toQt(removed.error);
        }
        return result;
    }
    emit keysChanged();
    return result;
}

auto KeysModule::cacheXpub(const QString &fingerprint, const QString &path, const QString &xpub)
    -> KeyOpResult {
    if (rust() == nullptr) {
        return failedOp(TR("keys-module-not-ready"));
    }
    auto result =
        rust()->cache_xpub(rust::String(fingerprint.toStdString()),
                           rust::String(path.toStdString()), rust::String(xpub.toStdString()));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::removeXpub(const QString &fingerprint, const QString &path) -> KeyOpResult {
    if (rust() == nullptr) {
        return failedOp(TR("keys-module-not-ready"));
    }
    auto result = rust()->remove_xpub(rust::String(fingerprint.toStdString()),
                                      rust::String(path.toStdString()));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::recordDevice(const plugin::SignerEntry &entry) -> KeyOpResult {
    if (rust() == nullptr) {
        return failedOp(TR("keys-module-not-ready"));
    }
    auto result = rust()->record_device(toLiveDevice(entry));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::recordQrDevice(const QString &fingerprint, const qr::QrDevice &device)
    -> KeyOpResult {
    if (rust() == nullptr) {
        return failedOp(TR("keys-module-not-ready"));
    }
    LiveDevice live;
    live.fingerprint = rust::String(fingerprint.toStdString());
    // As over USB, the device kind is its brand.
    live.kind = rust::String(device.vendor.toStdString());
    live.brand = rust::String(device.vendor.toStdString());
    live.model = rust::String(device.model.toStdString());
    live.version = rust::String(device.version.toStdString());
    live.capabilities = device.capabilities;
    live.module = rust::String("qr");
    auto result = rust()->record_device(std::move(live));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::coinType() -> int {
    return KeysModule::network() == Network::Bitcoin ? 0 : 1;
}

auto KeysModule::defaultXpubPath() -> QString {
    return QStringLiteral("m/84'/%1'/0'").arg(coinType());
}

auto KeysModule::accountXpubPaths() -> QStringList {
    int coin = coinType();
    return {defaultXpubPath(), QStringLiteral("m/86'/%1'/0'").arg(coin),
            QStringLiteral("m/48'/%1'/0'/2'").arg(coin), QStringLiteral("m/87'/%1'/0'").arg(coin)};
}

auto KeysModule::deviceXpubPaths() -> QStringList {
    int coin = coinType();
    return {defaultXpubPath(), QStringLiteral("m/86'/%1'/0'").arg(coin),
            QStringLiteral("m/87'/%1'/0'").arg(coin), QStringLiteral("m/352'/%1'/0'").arg(coin)};
}

auto KeysModule::toLiveDevice(const plugin::SignerEntry &entry) -> LiveDevice {
    LiveDevice device;
    device.device_id = rust::String(entry.instanceId.toStdString());
    device.fingerprint = rust::String(entry.signer->fingerprint().toStdString());
    // The device service reports the device kind as its brand.
    device.kind = rust::String(entry.signer->brand().toStdString());
    device.brand = rust::String(entry.signer->brand().toStdString());
    device.model = rust::String(entry.signer->model().toStdString());
    device.serial = rust::String(entry.signer->serial().toStdString());
    // USB reports no firmware version nor capabilities.
    device.capabilities = 0;
    if (entry.module != nullptr) {
        device.module =
            rust::String(entry.module->meta().value(QStringLiteral("id")).toStdString());
    }
    return device;
}

RustKeys *KeysModule::rust() const {
    return handle(activeNetwork());
}

auto KeysModule::activeNetwork() const -> Network {
    return m_scope.value_or(s_network);
}

auto KeysModule::handle(Network network) const -> RustKeys * {
    auto found = m_handles.find(network);
    if (found == m_handles.end()) {
        found = m_handles.emplace(network, ::new_keys_module(network)).first;
        if (!found->second->is_ok()) {
            qWarning() << "keys:" << toQt(found->second->get_error());
        }
        pushLiveDevices(*found->second);
    }
    return found->second.operator->();
}

void KeysModule::pushLiveDevices(RustKeys &handle) {
    rust::Vec<LiveDevice> devices;
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr) {
            devices.push_back(toLiveDevice(entry));
        }
    }
    handle.set_live_devices(std::move(devices));
}

NetworkScope::NetworkScope(KeysModule *module, Network network)
    : m_module(module),
      m_previous(module->m_scope) {
    m_module->m_scope = network;
}

NetworkScope::~NetworkScope() {
    m_module->m_scope = m_previous;
}

HwiModule *KeysModule::hwi() const {
    return m_hwi;
}

KeysBus *KeysModule::bus() const {
    return m_bus.get();
}

auto KeysModule::tab() -> QWidget * {
    if (m_tab.isNull()) {
        m_tab = new KeysTab(this);
    }
    return m_tab.data();
}

ReqId KeysModule::nextReqId() {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}

void KeysModule::bindHwiModule() {
    if (m_hwi != nullptr) {
        return;
    }
    for (const auto &entry : Host::get()->signers()) {
        auto *hwi = qobject_cast<HwiModule *>(entry.module);
        if (hwi == nullptr) {
            continue;
        }
        m_hwi = hwi;
        connect(m_hwi, &HwiModule::devicesChanged, this, &KeysModule::onDevicesChanged,
                qontrol::UNIQUE);
        // A network picked in the Keys tab before any device was seen reached only the
        // keys module: the hwi module follows it now.
        m_hwi->setNetwork(s_network);
        return;
    }
}

void KeysModule::refreshLiveDevices() {
    for (auto &[network, opened] : m_handles) {
        pushLiveDevices(*opened);
    }
    emit liveDevicesChanged();
}

void KeysModule::onInstanceRegistered(const QString &id) {
    Q_UNUSED(id);
    bindHwiModule();
    refreshLiveDevices();
}

void KeysModule::onInstanceRemoved(const QString &id) {
    for (auto it = m_device_registrations.begin(); it != m_device_registrations.end();) {
        if (it->instance_id == id) {
            m_bus->deliverError(it->bus_req, TR("keys-register-disconnected"));
            it = m_device_registrations.erase(it);
        } else {
            ++it;
        }
    }
    bindHwiModule();
    refreshLiveDevices();
}

void KeysModule::onDevicesChanged() {
    refreshLiveDevices();
}

void KeysModule::onDeviceIsRegistered(ReqId req_id, bool registered) {
    auto found = m_device_registrations.find(req_id);
    if (found == m_device_registrations.end() || found->signer != sender() || !found->checking) {
        return;
    }
    auto registration = m_device_registrations.take(req_id);
    if (registered) {
        finishDeviceRegistration(registration, std::nullopt);
        return;
    }
    registration.checking = false;
    sendDeviceRegistration(registration);
}

void KeysModule::onDeviceRegistered(ReqId req_id, bool registered) {
    auto found = m_device_registrations.find(req_id);
    if (found == m_device_registrations.end() || found->signer != sender() || found->checking) {
        return;
    }
    auto registration = m_device_registrations.take(req_id);
    if (registered) {
        finishDeviceRegistration(registration, std::nullopt);
    } else {
        finishDeviceRegistration(registration, TR("keys-register-account-refused"));
    }
}

void KeysModule::onDeviceRegisterError(std::optional<ReqId> req_id, const QString &message,
                                       [[maybe_unused]] plugin::SignError kind) {
    if (!req_id.has_value()) {
        return;
    }
    auto found = m_device_registrations.find(req_id.value());
    if (found == m_device_registrations.end() || found->signer != sender()) {
        return;
    }
    finishDeviceRegistration(m_device_registrations.take(req_id.value()), message);
}

} // namespace keys
