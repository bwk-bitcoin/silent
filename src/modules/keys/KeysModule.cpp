#include "KeysModule.h"

#include "KeysTab.h"
#include "modules/hwi/HwiModule.h"
#include "modules/keys/modals/PasswordPrompt.h"
#include "modules/keys/signers/OfflineSigner.h"
#include "modules/keys/signers/RouteSigner.h"

#include "AppController.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"

#include <QDebug>
#include <QDialog>
#include <QThread>

#include <algorithm>

#include <common.h>

namespace keys {

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

void KeysBus::sign(ReqId req_id, [[maybe_unused]] const QString &psbt_v2_hex,
                   [[maybe_unused]] const QStringList &descriptors) {
    deliverError(req_id, QStringLiteral("keys: signing is not wired yet"));
}

void KeysBus::verifyAddress(ReqId req_id, [[maybe_unused]] const QString &descriptor,
                            [[maybe_unused]] const QString &path,
                            [[maybe_unused]] const QString &address) {
    deliverError(req_id, QStringLiteral("keys: signing is not wired yet"));
}

void KeysBus::spDescriptor(ReqId req_id, [[maybe_unused]] const QString &fingerprint) {
    deliverError(req_id, QStringLiteral("keys: sp descriptor fetch is not wired yet"));
}

void KeysBus::registerDescriptor(ReqId req_id, [[maybe_unused]] const QString &fingerprint,
                                 [[maybe_unused]] const QString &descriptor) {
    deliverError(req_id, QStringLiteral("keys: descriptor registration is not wired yet"));
}

void KeysBus::deliverSigned(ReqId req_id, const QString &psbt_v2_hex) {
    emit signed_(req_id, psbt_v2_hex);
}

void KeysBus::deliverVerified(ReqId req_id, const QString &address) {
    emit verified(req_id, address);
}

void KeysBus::deliverError(std::optional<ReqId> req_id, const QString &message) {
    emit error(req_id, message);
}

KeysModule::KeysModule(QObject *parent) : IModule(parent) {
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
    if (cached != nullptr && hasPendingRequests(cached)) {
        // The route changed but a request on the stale instance is still in
        // flight: keep serving it until the caller holding it gets an answer.
        return cached;
    }

    auto *created = new RouteSigner(this, info, route, this);
    auto deferred = static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection);
    connect(created, &RouteSigner::routeSignRequested, this, &KeysModule::onRouteSignRequested,
            deferred);
    connect(created, &RouteSigner::routeDisplayRequested, this,
            &KeysModule::onRouteDisplayRequested, deferred);
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
    if (cached != nullptr && hasPendingRequests(cached)) {
        return cached;
    }

    auto *created = new OfflineSigner(this, info, this);
    if (cached != nullptr) {
        cached->deleteLater();
    }
    m_signers.insert(fingerprint, created);
    return created;
}

auto KeysModule::hasPendingRequests(const ISigner *signer) const -> bool {
    return std::ranges::any_of(m_pending_requests,
                               [signer](const ISigner *pending) { return pending == signer; });
}

void KeysModule::generateMnemonic(ReqId req_id, std::uint32_t word_count) {
    auto *thread = QThread::create([this, req_id, word_count]() {
        emit mnemonicGenerationFinished(req_id, ::generate_mnemonic(word_count));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onMnemonicGenerationFinished(ReqId req_id, MnemonicResult result) {
    emit mnemonicGenerated(req_id, result);
}

void KeysModule::importHotKey(ReqId req_id, QString mnemonic, QString alias, QString password) {
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

void KeysModule::onHotKeySealed(ReqId req_id, SealedHotKey *sealed, QString alias) {
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

void KeysModule::revealMnemonic(ReqId req_id, QString fingerprint, QString password) {
    if (rust() == nullptr) {
        MnemonicResult result;
        result.is_ok = false;
        result.error = rust::String("keys: module not ready");
        emit mnemonicRevealFinished(req_id, result);
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        MnemonicResult result;
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

void KeysModule::onMnemonicRevealFinished(ReqId req_id, MnemonicResult result) {
    emit mnemonicRevealed(req_id, result);
}

void KeysModule::signHot(ReqId req_id, QString fingerprint, QString password, QString psbt_v2_hex) {
    if (rust() == nullptr) {
        emit hotSignFinished(req_id, false, QString(), TR("keys-module-not-ready"));
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        emit hotSignFinished(req_id, false, QString(), toQt(material->get_error()));
        return;
    }
    auto *thread = QThread::create([this, req_id, material = material.into_raw(),
                                    password = password.toStdString(),
                                    psbt = psbt_v2_hex.toStdString()]() mutable {
        auto box = rust::Box<HotMaterial>::from_raw(material);
        auto result = ::hot_sign(*box, password, psbt);
        password.assign(password.size(), '\0');
        password.clear();
        emit hotSignFinished(req_id, result.is_ok, toQt(result.psbt_v2_hex), toQt(result.error));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onHotSignFinished(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error) {
    emit hotSigned(req_id, is_ok, psbt_v2_hex, error);
}

void KeysModule::trackPendingRequest(ReqId req_id, ISigner *signer) {
    m_pending_requests.insert(req_id, signer);
}

void KeysModule::untrackPendingRequest(ReqId req_id) {
    m_pending_requests.remove(req_id);
}

void KeysModule::promptPassword(RouteSigner *requester, ReqId req_id) {
    auto *prompt =
        new modal::PasswordPrompt(modal::PasswordPrompt::Mode::Use, TR("keys-password-use"));
    m_password_requests.insert(prompt, PasswordRequest{.requester = requester, .req_id = req_id});
    connect(prompt, &modal::PasswordPrompt::passwordEntered, this,
            &KeysModule::onPasswordPromptEntered, qontrol::UNIQUE);
    connect(prompt, &QDialog::rejected, this, &KeysModule::onPasswordPromptRejected,
            qontrol::UNIQUE);
    AppController::execModal(prompt);
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

RustKeys *KeysModule::rust() const {
    return m_rust.has_value() ? m_rust.value().operator->() : nullptr;
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

auto KeysModule::requestIdFor(ReqId signer_req_id) -> ReqId {
    ReqId requestId = m_signer_to_request.take(signer_req_id);
    return requestId == 0 ? signer_req_id : requestId;
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
        LiveDevice device;
        device.device_id = rust::String(entry.instanceId.toStdString());
        device.fingerprint = rust::String(entry.signer->fingerprint().toStdString());
        device.brand = rust::String(entry.signer->brand().toStdString());
        device.model = rust::String(entry.signer->model().toStdString());
        device.serial = rust::String(entry.signer->serial().toStdString());
        if (entry.module != nullptr) {
            device.module =
                rust::String(entry.module->meta().value(QStringLiteral("id")).toStdString());
        }
        devices.push_back(std::move(device));
    }
    m_rust.value()->set_live_devices(std::move(devices));
    emit liveDevicesChanged();
}

void KeysModule::onKeyEventReceived(const KeyEvent &event) {
    switch (event.flag) {
    case KeyEventFlag::Error:
        if (event.req_id == 0) {
            qWarning() << "keys:" << toQt(event.error);
            return;
        }
        m_bus->deliverError(event.req_id, toQt(event.error));
        return;
    default:
        // QrDecoded, ScanProgress and CameraStopped drive the QR/camera modals of a
        // later task; nothing in this skeleton reacts to them yet.
        break;
    }
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

void KeysModule::onPasswordPromptEntered(QString password) {
    PasswordRequest request = m_password_requests.take(sender());
    if (request.requester != nullptr) {
        request.requester->onPasswordResolved(request.req_id, password);
    }
    password.fill(QChar());
    password.clear();
}

void KeysModule::onPasswordPromptRejected() {
    PasswordRequest request = m_password_requests.take(sender());
    if (request.requester != nullptr) {
        request.requester->onPasswordResolved(request.req_id, QString());
    }
}

void KeysModule::onRouteSignRequested(ReqId req_id, [[maybe_unused]] const QString &fingerprint,
                                      [[maybe_unused]] const QString &psbt_v2_hex,
                                      [[maybe_unused]] const QString &descriptor) {
    auto *signer = qobject_cast<RouteSigner *>(sender());
    if (signer == nullptr) {
        return;
    }
    m_pending_requests.insert(req_id, signer);
    // The interactive QR and SD card flows are wired in a later task.
    signer->deliverError(req_id, TR("keys-route-unavailable"));
    m_pending_requests.remove(req_id);
}

void KeysModule::onRouteDisplayRequested(ReqId req_id, [[maybe_unused]] const QString &fingerprint,
                                         [[maybe_unused]] const QString &descriptor,
                                         [[maybe_unused]] const QString &path,
                                         [[maybe_unused]] const QString &address) {
    auto *signer = qobject_cast<RouteSigner *>(sender());
    if (signer == nullptr) {
        return;
    }
    m_pending_requests.insert(req_id, signer);
    // The interactive QR and SD card flows are wired in a later task.
    signer->deliverError(req_id, TR("keys-route-unavailable"));
    m_pending_requests.remove(req_id);
}

} // namespace keys
