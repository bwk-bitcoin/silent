#include "KeysModule.h"

#include "KeysTab.h"
#include "QrRoute.h"
#include "modules/hwi/HwiModule.h"
#include "modules/keys/flow/SigningFlow.h"
#include "modules/keys/signers/OfflineSigner.h"
#include "modules/keys/signers/RouteSigner.h"

#include "AppController.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"

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

} // namespace

KeysBus::KeysBus(KeysModule *module, QObject *parent) : IKeys(parent), m_module(module) {
}

QList<KeyEntry> KeysBus::keys(Network network) const {
    QList<KeyEntry> entries;
    // The module holds one network at a time, so a request for any other network
    // gets nothing rather than a stale cross-network list.
    if (network != KeysModule::network()) {
        return entries;
    }
    for (const auto &info : m_module->keys()) {
        entries.append(m_module->buildKeyEntry(info));
    }
    return entries;
}

auto KeysBus::newRequest() -> ReqId {
    return m_module->nextReqId();
}

void KeysBus::sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors,
                   const QString &wallet_name) {
    m_module->sign(req_id, psbt_v2_hex, descriptors, wallet_name);
}

void KeysBus::verifyAddress(ReqId req_id, const QString &descriptor, const QString &path,
                            const QString &address) {
    m_module->verifyAddress(req_id, descriptor, path, address);
}

void KeysBus::spDescriptor(ReqId req_id, const QString &fingerprint) {
    m_module->spDescriptor(req_id, fingerprint);
}

void KeysBus::registerDescriptor(ReqId req_id, const QString &fingerprint,
                                 const QString &descriptor) {
    m_module->registerDescriptor(req_id, fingerprint, descriptor);
}

void KeysBus::deliverSigned(ReqId req_id, const QString &psbt_v2_hex) {
    emit signed_(req_id, psbt_v2_hex);
}

void KeysBus::deliverVerified(ReqId req_id, bool matched) {
    emit verified(req_id, matched);
}

void KeysBus::deliverError(std::optional<ReqId> req_id, const QString &message) {
    emit error(req_id, message);
}

void KeysBus::deliverCancelled(ReqId req_id) {
    emit cancelled(req_id);
}

void KeysBus::deliverSpDescriptor(ReqId req_id, const QString &descriptor,
                                  const QString &fingerprint) {
    emit spDescriptorReady(req_id, descriptor, fingerprint);
}

void KeysBus::deliverDescriptorRegistered(ReqId req_id, const QString &fingerprint) {
    emit descriptorRegistered(req_id, fingerprint);
}

KeysModule::KeysModule(QObject *parent) : IModule(parent) {
    m_qr_route = new QrRoute(this);
    connect(this, &KeysModule::keyEventReceived, this, &KeysModule::onKeyEventReceived,
            qontrol::UNIQUE);
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
    connect(this, &KeysModule::cameraOpenFinished, this, &KeysModule::onCameraOpenFinished,
            qontrol::UNIQUE);
    createModule();
    m_bus = std::make_unique<KeysBus>(this);
    Host::get()->setKeys(m_bus.get(), this);
    bindHwiModule();
    refreshLiveDevices();
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
    m_rust = std::make_optional(::new_keys_module(KeysModule::network()));
    if (!m_rust.value()->is_ok()) {
        qWarning() << "keys:" << toQt(m_rust.value()->get_error());
    }

    auto receiver = m_rust.value()->take_receiver();
    // QThread::create plus a lambda is the endorsed way to sit on a blocking FFI call:
    // the loop only emits, and the queued connection lands the work on the GUI thread.
    m_notif_thread = QThread::create([this, recv = std::move(receiver)]() {
        while (true) {
            auto poll = recv->recv();
            if (!poll->is_some()) {
                break;
            }
            emit keyEventReceived(poll->get_event());
        }
    });
    connect(m_notif_thread, &QThread::finished, m_notif_thread, &QThread::deleteLater,
            qontrol::UNIQUE);
    m_notif_thread->start();
}

void KeysModule::teardownModule() {
    if (m_rust.has_value()) {
        // Wakes the blocking recv with a sentinel; waiting for the sender to drop
        // would hang here for as long as an in-flight operation takes.
        m_rust.value()->shutdown();
    }
    if (m_notif_thread != nullptr) {
        m_notif_thread->wait();
        m_notif_thread = nullptr;
    }
    m_rust.reset();
}

Network KeysModule::network() {
    // The device service network is the application network today, and the Keys tab
    // drives both selectors from one combobox.
    return ::hwi_config_network();
}

void KeysModule::setNetwork(Network network) {
    if (m_synced_network == network) {
        return;
    }
    if (!m_rust.has_value()) {
        return;
    }
    // Does not touch hwi_set_config_network: the tab calls HwiModule::setNetwork for
    // that, which owns the persistence.
    auto result = m_rust.value()->set_network(network);
    if (!result.is_ok) {
        qWarning() << "keys:" << toQt(result.error);
        return;
    }
    m_synced_network = network;
    emit keysChanged();
}

QList<KeyInfo> KeysModule::keys() const {
    QList<KeyInfo> keys;
    if (!m_rust.has_value()) {
        return keys;
    }
    for (const auto &key : m_rust.value()->keys()) {
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
    auto *cached = m_signers.value(fingerprint, nullptr);
    auto *cachedRoute = qobject_cast<RouteSigner *>(cached);
    if (cachedRoute != nullptr && cachedRoute->route() == route) {
        return cachedRoute;
    }
    auto *created = new RouteSigner(this, info, route, this);
    if (cached != nullptr) {
        cached->deleteLater();
    }
    m_signers.insert(fingerprint, created);
    return created;
}

auto KeysModule::offlineSignerFor(const QString &fingerprint, const KeyInfo &info) -> ISigner * {
    auto *cached = m_signers.value(fingerprint, nullptr);
    if (qobject_cast<OfflineSigner *>(cached) != nullptr) {
        return cached;
    }
    auto *created = new OfflineSigner(this, info, this);
    if (cached != nullptr) {
        cached->deleteLater();
    }
    m_signers.insert(fingerprint, created);
    return created;
}

void KeysModule::runFlow(flow::SigningFlow *flow) {
    connect(flow, &flow::SigningFlow::psbtSigned, m_bus.get(), &KeysBus::deliverSigned,
            qontrol::UNIQUE);
    connect(flow, &flow::SigningFlow::addressVerified, m_bus.get(), &KeysBus::deliverVerified,
            qontrol::UNIQUE);
    connect(flow, &flow::SigningFlow::descriptorFetched, m_bus.get(), &KeysBus::deliverSpDescriptor,
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
    m_bus->deliverDescriptorRegistered(req_id, fingerprint);
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
        emit hotSignFinished(req_id, false, QString(), TR("keys-module-not-ready"), false);
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        emit hotSignFinished(req_id, false, QString(), toQt(material->get_error()), false);
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
                             result.wrong_password);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onHotSignFinished(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error,
                                   bool wrong_password) {
    emit hotSigned(req_id, is_ok, std::move(psbt_v2_hex), std::move(error), wrong_password);
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

void KeysModule::openCamera(ReqId req_id, std::uint32_t index) {
    m_camera_req_id = req_id;
    auto *thread = QThread::create([this, req_id, index]() {
        auto handle = ::camera_open(index);
        emit cameraOpenFinished(req_id, handle.into_raw());
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::cancelCamera() {
    m_camera_req_id = 0;
    if (m_camera_installed && rust() != nullptr) {
        rust()->camera_stop();
        m_camera_installed = false;
    }
}

void KeysModule::onCameraOpenFinished(ReqId req_id, CameraHandle *handle) {
    auto box = rust::Box<CameraHandle>::from_raw(handle);
    if (req_id != m_camera_req_id) {
        // A newer openCamera() or a cancelCamera() call superseded this one;
        // dropping `box` here releases the device instead of installing a stale
        // handle.
        return;
    }
    if (!box->is_ok()) {
        emit cameraOpened(req_id, false, toQt(box->get_error()));
        return;
    }
    if (rust() == nullptr) {
        emit cameraOpened(req_id, false, TR("keys-module-not-ready"));
        return;
    }
    auto result = rust()->camera_install(std::move(box));
    if (!result.is_ok) {
        emit cameraOpened(req_id, false, toQt(result.error));
        return;
    }
    m_camera_installed = true;
    emit cameraOpened(req_id, true, QString());
}

auto KeysModule::setAlias(const QString &fingerprint, const QString &alias) -> KeyOpResult {
    if (!m_rust.has_value()) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = m_rust.value()->set_alias(rust::String(fingerprint.toStdString()),
                                            rust::String(alias.toStdString()));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::addRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult {
    if (!m_rust.has_value()) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = m_rust.value()->add_route(rust::String(fingerprint.toStdString()), kind);
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::removeRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult {
    if (!m_rust.has_value()) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = m_rust.value()->remove_route(rust::String(fingerprint.toStdString()), kind);
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::setRouteOrder(const QString &fingerprint, rust::Vec<KeyRouteKind> order)
    -> KeyOpResult {
    if (!m_rust.has_value()) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result =
        m_rust.value()->set_route_order(rust::String(fingerprint.toStdString()), std::move(order));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::removeKey(const QString &fingerprint, bool force) -> KeyOpResult {
    if (!m_rust.has_value()) {
        KeyOpResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        return result;
    }
    auto result = m_rust.value()->remove_key(rust::String(fingerprint.toStdString()), force);
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

auto KeysModule::insertKey(const QString &fingerprint, const QString &alias,
                           const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs)
    -> KeyOpResult {
    if (!m_rust.has_value()) {
        return failedOp(TR("keys-module-not-ready"));
    }
    auto key = rust::String(fingerprint.toStdString());
    auto added = m_rust.value()->add_key(key, rust::String(alias.toStdString()));
    if (!added.is_ok) {
        return failedOp(toQt(added.error));
    }
    KeyOpResult result{.is_ok = true, .error = rust::String()};
    for (auto route : routes) {
        if (result.is_ok) {
            result = m_rust.value()->add_route(key, route);
        }
    }
    for (const auto &entry : xpubs) {
        if (result.is_ok) {
            result = m_rust.value()->cache_xpub(key, rust::String(entry.path.toStdString()),
                                                rust::String(entry.xpub.toStdString()));
        }
    }
    if (!result.is_ok) {
        // A key half added would show up without its routes or xpubs.
        auto removed = m_rust.value()->remove_key(key, true);
        if (!removed.is_ok) {
            qWarning() << "keys:" << toQt(removed.error);
        }
    }
    return result;
}

auto KeysModule::addDeviceKey(const plugin::SignerEntry &entry, const QString &alias,
                              const QString &path, const QString &xpub) -> KeyOpResult {
    QString fingerprint = entry.signer->fingerprint();
    auto result =
        insertKey(fingerprint, alias, {KeyRouteKind::Usb}, {{.path = path, .xpub = xpub}});
    if (!result.is_ok) {
        return result;
    }
    result = m_rust.value()->record_device(toLiveDevice(entry));
    if (!result.is_ok) {
        auto removed = m_rust.value()->remove_key(rust::String(fingerprint.toStdString()), true);
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
    if (!m_rust.has_value()) {
        return failedOp(TR("keys-module-not-ready"));
    }
    auto result = m_rust.value()->cache_xpub(rust::String(fingerprint.toStdString()),
                                             rust::String(path.toStdString()),
                                             rust::String(xpub.toStdString()));
    if (result.is_ok) {
        emit keysChanged();
    }
    return result;
}

auto KeysModule::recordDevice(const plugin::SignerEntry &entry) -> KeyOpResult {
    if (!m_rust.has_value()) {
        return failedOp(TR("keys-module-not-ready"));
    }
    auto result = m_rust.value()->record_device(toLiveDevice(entry));
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

auto KeysModule::toLiveDevice(const plugin::SignerEntry &entry) -> LiveDevice {
    LiveDevice device;
    device.device_id = rust::String(entry.instanceId.toStdString());
    device.fingerprint = rust::String(entry.signer->fingerprint().toStdString());
    // The device service reports the device kind as its brand.
    device.kind = rust::String(entry.signer->brand().toStdString());
    device.brand = rust::String(entry.signer->brand().toStdString());
    device.model = rust::String(entry.signer->model().toStdString());
    device.serial = rust::String(entry.signer->serial().toStdString());
    if (entry.module != nullptr) {
        device.module =
            rust::String(entry.module->meta().value(QStringLiteral("id")).toStdString());
    }
    return device;
}

RustKeys *KeysModule::rust() const {
    return m_rust.has_value() ? m_rust.value().operator->() : nullptr;
}

HwiModule *KeysModule::hwi() const {
    return m_hwi;
}

KeysBus *KeysModule::bus() const {
    return m_bus.get();
}

auto KeysModule::qrRoute() const -> QrRoute * {
    return m_qr_route;
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
        return;
    }
}

void KeysModule::refreshLiveDevices() {
    if (!m_rust.has_value()) {
        emit liveDevicesChanged();
        return;
    }
    rust::Vec<LiveDevice> devices;
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer == nullptr) {
            continue;
        }
        devices.push_back(toLiveDevice(entry));
    }
    m_rust.value()->set_live_devices(std::move(devices));
    emit liveDevicesChanged();
}

void KeysModule::onKeyEventReceived(const KeyEvent &event) {
    m_qr_route->onKeyEvent(event);
}

void KeysModule::onInstanceRegistered(const QString &id) {
    Q_UNUSED(id);
    bindHwiModule();
    refreshLiveDevices();
}

void KeysModule::onInstanceRemoved(const QString &id) {
    Q_UNUSED(id);
    bindHwiModule();
    refreshLiveDevices();
}

void KeysModule::onDevicesChanged() {
    refreshLiveDevices();
}

} // namespace keys
