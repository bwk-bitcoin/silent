#include "KeysModule.h"

#include "KeysTab.h"
#include "QrRoute.h"
#include "modules/hwi/HwiModule.h"
#include "modules/keys/modals/PasswordPrompt.h"
#include "modules/keys/modals/SdPicker.h"
#include "modules/keys/modals/SignerPicker.h"
#include "modules/keys/modals/UsbConfirm.h"
#include "modules/keys/signers/OfflineSigner.h"
#include "modules/keys/signers/RouteSigner.h"

#include "AppController.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"

#include <QByteArray>
#include <QDebug>
#include <QDialog>
#include <QThread>

#include <algorithm>
#include <cstdint>

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

void KeysBus::sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors) {
    m_module->sign(req_id, psbt_v2_hex, descriptors);
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
    connect(this, &KeysModule::sdLoadReady, this, &KeysModule::onSdLoadReady, qontrol::UNIQUE);
    // Queued on purpose, mirroring RouteSigner::routeSignRequested: a result must
    // not land before spDescriptor() has returned the ReqId to its caller.
    connect(this, &KeysModule::qrSpDescriptorRequested, this,
            &KeysModule::onQrSpDescriptorRequested,
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
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
    // Forwarded to the bus for every route this class drives (hot, qr, sd card),
    // whichever request happens to be in flight; the bus's callers filter by ReqId.
    connect(created, &ISigner::signed_, this, &KeysModule::onRouteSignSigned, qontrol::UNIQUE);
    connect(created, &ISigner::error, this, &KeysModule::onRouteSignError, qontrol::UNIQUE);
    connect(created, &RouteSigner::spDescriptorFetched, this,
            &KeysModule::onRouteSpDescriptorFetched, qontrol::UNIQUE);
    connect(created, &RouteSigner::addressVerified, this, &KeysModule::onRouteVerifyDisplayed,
            qontrol::UNIQUE);
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

auto KeysModule::showSignerPicker(const QString &title, const SigningPlan &plan) -> QString {
    auto *picker = new modal::SignerPicker(title, plan);
    connect(picker, &modal::SignerPicker::picked, this, &KeysModule::onSignerPicked,
            qontrol::UNIQUE);
    m_picked_fingerprint.clear();
    AppController::execModal(picker);
    return m_picked_fingerprint;
}

auto KeysModule::signingPlanFor(const QString &psbt_v2_hex) const -> SigningPlan {
    if (rust() == nullptr) {
        SigningPlan plan;
        plan.is_ok = false;
        plan.error = rust::String("keys: module not ready");
        AppController::execModal(new qontrol::Modal(TR("common-error"), toQt(plan.error)));
        return plan;
    }
    // signing_plan() blocks on nothing (no device I/O), so calling it from the GUI
    // thread is safe: see its doc comment in lib/include/silent.h.
    auto plan = rust()->signing_plan(rust::String(psbt_v2_hex.toStdString()));
    if (!plan.is_ok) {
        AppController::execModal(new qontrol::Modal(TR("common-error"), toQt(plan.error)));
    }
    return plan;
}

void KeysModule::sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors) {
    auto plan = signingPlanFor(psbt_v2_hex);
    if (!plan.is_ok) {
        m_bus->deliverError(req_id, toQt(plan.error));
        return;
    }

    QString fingerprint = showSignerPicker(TR("send-select-signer"), plan);
    if (fingerprint.isEmpty()) {
        m_bus->deliverError(req_id, TR("keys-signing-cancelled"));
        return;
    }

    QString descriptor = descriptors.isEmpty() ? QString() : descriptors.first();
    for (const auto &info : keys()) {
        if (QString::compare(toQt(info.fingerprint), fingerprint, Qt::CaseInsensitive) != 0) {
            continue;
        }
        auto entry = buildKeyEntry(info);
        if (!entry.can_sign || entry.signer == nullptr) {
            break;
        }
        auto *route = qobject_cast<RouteSigner *>(entry.signer);
        if (route != nullptr) {
            // Hot/Qr/SdCard all expect v2 hex, not base64: signWithDescriptor()'s
            // parameter name mirrors the generic ISigner interface, but RouteSigner
            // forwards it straight to Rust or into the SD card/QR modal flow. It
            // returns its own ReqId, mapped back to req_id for the answer.
            ReqId signerReqId = route->signWithDescriptor(psbt_v2_hex, descriptor);
            m_signer_to_request.insert(signerReqId, req_id);
            return;
        }
        // Not a RouteSigner and can_sign is true: the module's own OfflineSigner
        // never reports can_sign, so this is a live device on the Usb route.
        signWithDevice(entry.signer, psbt_v2_hex, descriptor, req_id);
        return;
    }

    m_bus->deliverError(req_id, TR("keys-route-unavailable"));
}

auto KeysModule::verifyUnusableReason(const KeyInfo &info) -> KeyUnusableReason {
    if (info.routes.empty()) {
        return KeyUnusableReason::NoRoute;
    }
    if (info.routes.size() == 1 && info.routes[0] == KeyRouteKind::Hot && !info.has_hot_secret) {
        return KeyUnusableReason::NoHotSecret;
    }
    return KeyUnusableReason::DeviceNotConnected;
}

auto KeysModule::buildVerifyCandidate(const KeyInfo &info) -> SigningCandidate {
    SigningCandidate candidate;
    candidate.fingerprint = info.fingerprint;
    candidate.alias = info.alias;
    candidate.registered = true;
    candidate.must_sign_first = false;

    auto entry = buildKeyEntry(info);
    candidate.can_sign = entry.can_sign;
    if (!entry.can_sign) {
        // Meaningless when can_sign is false; Hot is the sentinel value (see
        // SigningCandidate::route).
        candidate.route = KeyRouteKind::Hot;
        candidate.unusable = verifyUnusableReason(info);
        return candidate;
    }

    candidate.unusable = KeyUnusableReason::None;
    auto *route = qobject_cast<RouteSigner *>(entry.signer);
    if (route != nullptr) {
        candidate.route = route->route();
        return candidate;
    }
    // Not a RouteSigner and can_sign is true: the module's own OfflineSigner never
    // reports can_sign, so this is a live device on the Usb route.
    candidate.route = KeyRouteKind::Usb;
    if (entry.signer != nullptr) {
        candidate.device_id = rust::String(entry.signer->id().toStdString());
    }
    return candidate;
}

void KeysModule::verifyAddress(ReqId req_id, const QString &descriptor, const QString &path,
                               const QString &address) {
    if (rust() == nullptr) {
        m_bus->deliverError(req_id, TR("keys-module-not-ready"));
        return;
    }

    auto candidateKeys = rust()->keys_for_descriptor(rust::String(descriptor.toStdString()));
    if (candidateKeys.empty()) {
        m_bus->deliverError(req_id, TR("keys-route-unavailable"));
        return;
    }

    SigningPlan plan;
    plan.is_ok = true;
    plan.sp_outputs_pending = false;
    for (const auto &info : candidateKeys) {
        plan.candidates.push_back(buildVerifyCandidate(info));
    }

    QString fingerprint = showSignerPicker(TR("receive-verify-title"), plan);
    if (fingerprint.isEmpty()) {
        m_bus->deliverError(req_id, TR("keys-signing-cancelled"));
        return;
    }

    for (const auto &info : candidateKeys) {
        if (QString::compare(toQt(info.fingerprint), fingerprint, Qt::CaseInsensitive) != 0) {
            continue;
        }
        auto entry = buildKeyEntry(info);
        if (!entry.can_sign || entry.signer == nullptr) {
            break;
        }
        auto *route = qobject_cast<RouteSigner *>(entry.signer);
        if (route != nullptr) {
            ReqId signerReqId = route->displayAddress(descriptor, path, address);
            m_signer_to_request.insert(signerReqId, req_id);
            return;
        }
        // Not a RouteSigner and can_sign is true: the module's own OfflineSigner
        // never reports can_sign, so this is a live device on the Usb route.
        verifyWithDevice(entry.signer, descriptor, path, address, req_id);
        return;
    }

    m_bus->deliverError(req_id, TR("keys-route-unavailable"));
}

void KeysModule::spDescriptor(ReqId req_id, const QString &fingerprint) {
    if (rust() == nullptr) {
        m_bus->deliverError(req_id, TR("keys-sp-descriptor-unavailable"));
        return;
    }

    auto infos = keys();
    auto found = std::ranges::find_if(infos, [&fingerprint](const KeyInfo &info) {
        return QString::compare(toQt(info.fingerprint), fingerprint, Qt::CaseInsensitive) == 0;
    });
    if (found == infos.end()) {
        m_bus->deliverError(req_id, TR("keys-sp-descriptor-unavailable"));
        return;
    }

    auto *route = qobject_cast<RouteSigner *>(buildKeyEntry(*found).signer);
    if (route == nullptr) {
        m_bus->deliverError(req_id, TR("keys-sp-descriptor-unavailable"));
        return;
    }

    switch (route->route()) {
    case KeyRouteKind::Hot:
        m_sp_descriptor_fingerprints.insert(req_id, fingerprint);
        route->fetchSpDescriptor(req_id);
        return;
    case KeyRouteKind::Qr:
        // Queued (like sign()'s Qr/SdCard path via routeSignRequested): building
        // the QR request opens QrDisplay/CameraScan modals synchronously, so this
        // must not run until control has returned to the top of the event loop.
        m_sp_descriptor_fingerprints.insert(req_id, fingerprint);
        emit qrSpDescriptorRequested(req_id);
        return;
    case KeyRouteKind::Usb:
    case KeyRouteKind::SdCard:
    case KeyRouteKind::Unknown:
    default:
        m_bus->deliverError(req_id, TR("keys-sp-descriptor-unavailable"));
        return;
    }
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

void KeysModule::fetchHotSpDescriptor(ReqId req_id, QString fingerprint, QString password) {
    if (rust() == nullptr) {
        emit hotSpDescriptorFinished(req_id, false, QString(), TR("keys-module-not-ready"));
        return;
    }
    auto material = rust()->hot_material(rust::String(fingerprint.toStdString()));
    if (!material->is_ok()) {
        emit hotSpDescriptorFinished(req_id, false, QString(), toQt(material->get_error()));
        return;
    }
    auto *thread = QThread::create([this, req_id, material = material.into_raw(),
                                    password = password.toStdString()]() mutable {
        auto box = rust::Box<HotMaterial>::from_raw(material);
        auto result = ::hot_sp_descriptor(*box, password);
        password.assign(password.size(), '\0');
        password.clear();
        emit hotSpDescriptorFinished(req_id, result.is_ok, toQt(result.descriptor),
                                     toQt(result.error));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onHotSpDescriptorFinished(ReqId req_id, bool is_ok, QString descriptor,
                                           QString error) {
    emit hotSpDescriptorFetched(req_id, is_ok, descriptor, error);
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

void KeysModule::validateAndDeliverSpDescriptor(ReqId req_id, const QString &descriptor) {
    auto expected = m_sp_descriptor_fingerprints.take(req_id);
    auto validated = ::validate_sp_descriptor_origin(rust::String(descriptor.toStdString()));
    if (!validated.is_ok) {
        m_bus->deliverError(req_id, TR("keys-sp-descriptor-no-origin"));
        return;
    }
    if (QString::compare(expected, toQt(validated.fingerprint), Qt::CaseInsensitive) != 0) {
        m_bus->deliverError(req_id, TR("keys-sp-descriptor-no-origin"));
        return;
    }
    m_bus->deliverSpDescriptor(req_id, toQt(validated.descriptor), toQt(validated.fingerprint));
}

void KeysModule::deliverSpDescriptorError(ReqId req_id, const QString &message) {
    m_sp_descriptor_fingerprints.remove(req_id);
    m_bus->deliverError(req_id, message);
}

void KeysModule::signWithDevice(ISigner *signer, const QString &psbt_v2_hex,
                                const QString &descriptor, ReqId req_id) {
    auto payload = ::usb_sign_payload(rust::String(psbt_v2_hex.toStdString()));
    if (!payload.is_ok) {
        m_bus->deliverError(req_id, toQt(payload.error));
        return;
    }

    connect(signer, &ISigner::signed_, this, &KeysModule::onDeviceSignSigned, qontrol::UNIQUE);
    connect(signer, &ISigner::error, this, &KeysModule::onDeviceSignError, qontrol::UNIQUE);

    ReqId signerReqId = signer->signWithDescriptor(toQt(payload.psbt_base64), descriptor);
    m_usb_sign_req_id = signerReqId;
    m_usb_signer = signer;
    m_usb_sign_psbt_v2_hex = psbt_v2_hex;
    m_signer_to_request.insert(signerReqId, req_id);

    auto *confirm =
        new modal::UsbConfirm(signer->id(), TR("confirm-send-device-step").arg(signer->id()));
    m_usb_confirm = confirm;
    connect(confirm, &modal::UsbConfirm::cancelled, this, &KeysModule::onUsbConfirmCancelled,
            qontrol::UNIQUE);
    AppController::execModal(confirm);
}

void KeysModule::verifyWithDevice(ISigner *signer, const QString &descriptor, const QString &path,
                                  const QString &address, ReqId req_id) {
    connect(signer, &ISigner::addressDisplayed, this, &KeysModule::onDeviceVerifyDisplayed,
            qontrol::UNIQUE);
    connect(signer, &ISigner::error, this, &KeysModule::onDeviceVerifyError, qontrol::UNIQUE);

    ReqId signerReqId = signer->displayAddress(descriptor, path, address);
    m_usb_verify_req_id = signerReqId;
    m_usb_verify_signer = signer;
    m_signer_to_request.insert(signerReqId, req_id);

    auto *confirm =
        new modal::UsbConfirm(signer->id(), TR("receive-verify-message").arg(signer->id()));
    m_usb_verify_confirm = confirm;
    connect(confirm, &modal::UsbConfirm::cancelled, this, &KeysModule::onUsbVerifyCancelled,
            qontrol::UNIQUE);
    AppController::execModal(confirm);
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

void KeysModule::onRouteSignRequested(ReqId req_id, const QString &fingerprint,
                                      const QString &psbt_v2_hex, const QString &descriptor) {
    auto *signer = qobject_cast<RouteSigner *>(sender());
    if (signer == nullptr) {
        return;
    }
    m_pending_requests.insert(req_id, signer);

    if (signer->route() == KeyRouteKind::SdCard) {
        QString alias = fingerprint;
        for (const auto &info : keys()) {
            if (QString::compare(toQt(info.fingerprint), fingerprint, Qt::CaseInsensitive) == 0 &&
                !toQt(info.alias).isEmpty()) {
                alias = toQt(info.alias);
                break;
            }
        }
        m_sd_request = SdRequest{.req_id = req_id, .signer = signer, .psbt_hex = psbt_v2_hex};
        auto *picker = new modal::SdPicker(alias);
        connect(picker, &modal::SdPicker::saveRequested, this, &KeysModule::onSdSaveRequested,
                qontrol::UNIQUE);
        connect(picker, &modal::SdPicker::loadRequested, this, &KeysModule::onSdLoadRequested,
                qontrol::UNIQUE);
        connect(picker, &modal::SdPicker::cancelled, this, &KeysModule::onSdPickerCancelled,
                qontrol::UNIQUE);
        connect(this, &KeysModule::sdSaveReady, picker, &modal::SdPicker::onSaveDone,
                qontrol::UNIQUE);
        connect(this, &KeysModule::sdLoadDone, picker, &modal::SdPicker::onLoadDone,
                qontrol::UNIQUE);
        AppController::execModal(picker);
        return;
    }

    m_qr_route->startSign(req_id, signer, psbt_v2_hex, descriptor);
}

void KeysModule::onRouteDisplayRequested(ReqId req_id, [[maybe_unused]] const QString &fingerprint,
                                         const QString &descriptor, const QString &path,
                                         const QString &address) {
    auto *signer = qobject_cast<RouteSigner *>(sender());
    if (signer == nullptr) {
        return;
    }
    m_pending_requests.insert(req_id, signer);

    m_qr_route->startVerify(req_id, signer, descriptor, path, address);
}

void KeysModule::onSignerPicked(QString fingerprint) {
    m_picked_fingerprint = std::move(fingerprint);
}

void KeysModule::onSdSaveRequested(QString path) {
    auto psbt = m_sd_request.psbt_hex.toStdString();
    auto *thread = QThread::create([this, path = path.toStdString(), psbt]() {
        auto result = ::sd_save_psbt(rust::String(path), rust::String(psbt));
        emit sdSaveReady(result.is_ok, toQt(result.error));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onSdLoadRequested(QString path) {
    auto *thread = QThread::create([this, path = path.toStdString()]() {
        auto result = ::sd_load_psbt(rust::String(path));
        emit sdLoadReady(result.is_ok, toQt(result.psbt_hex), toQt(result.error));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void KeysModule::onSdLoadReady(bool is_ok, QString psbt_hex, QString error) {
    QPointer<RouteSigner> signer = m_sd_request.signer;
    ReqId reqId = m_sd_request.req_id;
    m_sd_request = SdRequest{};
    if (signer.isNull()) {
        return;
    }
    m_pending_requests.remove(reqId);
    if (!is_ok) {
        signer->deliverError(reqId, error);
        emit sdLoadDone(false, error);
        return;
    }
    signer->deliverSigned(reqId, psbt_hex);
    emit sdLoadDone(true, QString());
}

void KeysModule::onSdPickerCancelled() {
    QPointer<RouteSigner> signer = m_sd_request.signer;
    if (signer.isNull()) {
        return;
    }
    ReqId reqId = m_sd_request.req_id;
    m_sd_request = SdRequest{};
    m_pending_requests.remove(reqId);
    signer->deliverError(reqId, TR("keys-signing-cancelled"));
}

void KeysModule::onQrSpDescriptorRequested(ReqId req_id) {
    m_qr_route->startSpDescriptor(req_id);
}

void KeysModule::onUsbConfirmCancelled() {
    if (m_usb_sign_req_id == 0) {
        return;
    }
    if (!m_usb_signer.isNull()) {
        disconnect(m_usb_signer, &ISigner::signed_, this, &KeysModule::onDeviceSignSigned);
        disconnect(m_usb_signer, &ISigner::error, this, &KeysModule::onDeviceSignError);
    }
    ReqId signerReqId = m_usb_sign_req_id;
    m_usb_signer = nullptr;
    m_usb_sign_req_id = 0;
    m_usb_sign_psbt_v2_hex.clear();
    m_bus->deliverError(requestIdFor(signerReqId), TR("keys-signing-cancelled"));
}

void KeysModule::onDeviceSignSigned(ReqId req_id, QString psbt_base64) {
    if (req_id != m_usb_sign_req_id) {
        return;
    }
    if (!m_usb_signer.isNull()) {
        disconnect(m_usb_signer, &ISigner::signed_, this, &KeysModule::onDeviceSignSigned);
        disconnect(m_usb_signer, &ISigner::error, this, &KeysModule::onDeviceSignError);
    }
    m_usb_signer = nullptr;
    m_usb_sign_req_id = 0;
    QString originalPsbtV2Hex = m_usb_sign_psbt_v2_hex;
    m_usb_sign_psbt_v2_hex.clear();
    ReqId requestId = requestIdFor(req_id);

    auto merged = ::usb_merge_signed(rust::String(originalPsbtV2Hex.toStdString()),
                                     rust::String(psbt_base64.toStdString()));
    if (!merged.is_ok) {
        if (!m_usb_confirm.isNull()) {
            m_usb_confirm->onFailed(toQt(merged.error));
        }
        m_bus->deliverError(requestId, toQt(merged.error));
        return;
    }

    if (!m_usb_confirm.isNull()) {
        m_usb_confirm->onSucceeded();
    }
    m_bus->deliverSigned(requestId, toQt(merged.psbt_v2_hex));
}

void KeysModule::onDeviceSignError(std::optional<ReqId> req_id, QString message) {
    if (!req_id.has_value() || req_id.value() != m_usb_sign_req_id) {
        return;
    }
    if (!m_usb_signer.isNull()) {
        disconnect(m_usb_signer, &ISigner::signed_, this, &KeysModule::onDeviceSignSigned);
        disconnect(m_usb_signer, &ISigner::error, this, &KeysModule::onDeviceSignError);
    }
    m_usb_signer = nullptr;
    m_usb_sign_req_id = 0;
    m_usb_sign_psbt_v2_hex.clear();
    ReqId requestId = requestIdFor(req_id.value());
    if (!m_usb_confirm.isNull()) {
        m_usb_confirm->onFailed(message);
    }
    m_bus->deliverError(requestId, message);
}

void KeysModule::onUsbVerifyCancelled() {
    if (m_usb_verify_req_id == 0) {
        return;
    }
    if (!m_usb_verify_signer.isNull()) {
        disconnect(m_usb_verify_signer, &ISigner::addressDisplayed, this,
                   &KeysModule::onDeviceVerifyDisplayed);
        disconnect(m_usb_verify_signer, &ISigner::error, this, &KeysModule::onDeviceVerifyError);
    }
    ReqId signerReqId = m_usb_verify_req_id;
    m_usb_verify_signer = nullptr;
    m_usb_verify_req_id = 0;
    m_bus->deliverError(requestIdFor(signerReqId), TR("keys-signing-cancelled"));
}

void KeysModule::onDeviceVerifyDisplayed(ReqId req_id, [[maybe_unused]] QString address) {
    if (req_id != m_usb_verify_req_id) {
        return;
    }
    if (!m_usb_verify_signer.isNull()) {
        disconnect(m_usb_verify_signer, &ISigner::addressDisplayed, this,
                   &KeysModule::onDeviceVerifyDisplayed);
        disconnect(m_usb_verify_signer, &ISigner::error, this, &KeysModule::onDeviceVerifyError);
    }
    m_usb_verify_signer = nullptr;
    m_usb_verify_req_id = 0;
    ReqId requestId = requestIdFor(req_id);
    if (!m_usb_verify_confirm.isNull()) {
        m_usb_verify_confirm->onSucceeded();
    }
    // bwk-hwi's HWI trait only ever returns Ok(()) or an Err from display_address;
    // there is no "the device showed it, but the user rejected the match" answer
    // for USB devices, unlike the Qr route's explicit verify_address result. A
    // device-side rejection surfaces as an Err instead, handled by
    // onDeviceVerifyError below, indistinguishable there from a transport failure.
    m_bus->deliverVerified(requestId, true);
}

void KeysModule::onDeviceVerifyError(std::optional<ReqId> req_id, QString message) {
    if (!req_id.has_value() || req_id.value() != m_usb_verify_req_id) {
        return;
    }
    if (!m_usb_verify_signer.isNull()) {
        disconnect(m_usb_verify_signer, &ISigner::addressDisplayed, this,
                   &KeysModule::onDeviceVerifyDisplayed);
        disconnect(m_usb_verify_signer, &ISigner::error, this, &KeysModule::onDeviceVerifyError);
    }
    m_usb_verify_signer = nullptr;
    m_usb_verify_req_id = 0;
    ReqId requestId = requestIdFor(req_id.value());
    if (!m_usb_verify_confirm.isNull()) {
        m_usb_verify_confirm->onFailed(message);
    }
    // bwk-hwi's Error::UserRefused (a device-side rejection) is collapsed into a
    // plain string alongside every other failure at the service's error boundary
    // (bwk hwi/src/service.rs, SigningDeviceMsg::Error), so this can be a device
    // refusal or an unrelated transport failure and there is no way to tell them
    // apart on this route. Route it through error() like onDeviceSignError does
    // for USB sign failures, instead of fabricating a verified(false) mismatch
    // that IKeys::verified's contract reserves for a genuine comparison result.
    qWarning() << "keys: usb verify failed:" << message;
    m_bus->deliverError(requestId, message);
}

void KeysModule::onRouteSignSigned(ReqId req_id, QString psbt_v2_hex) {
    m_bus->deliverSigned(requestIdFor(req_id), psbt_v2_hex);
}

void KeysModule::onRouteSignError(std::optional<ReqId> req_id, QString message) {
    if (!req_id.has_value()) {
        m_bus->deliverError(std::nullopt, message);
        return;
    }
    m_sp_descriptor_fingerprints.remove(*req_id);
    m_bus->deliverError(requestIdFor(*req_id), message);
}

void KeysModule::onRouteVerifyDisplayed(ReqId req_id, bool matched) {
    m_bus->deliverVerified(requestIdFor(req_id), matched);
}

void KeysModule::onRouteSpDescriptorFetched(ReqId req_id, bool is_ok, QString descriptor,
                                            QString error) {
    if (!is_ok) {
        m_sp_descriptor_fingerprints.remove(req_id);
        m_bus->deliverError(req_id, error);
        return;
    }
    validateAndDeliverSpDescriptor(req_id, descriptor);
}

} // namespace keys
