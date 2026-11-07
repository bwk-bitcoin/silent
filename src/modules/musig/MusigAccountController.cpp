#include "modules/musig/MusigAccountController.h"

#include "AppController.h"
#include "account/InterfaceEnums.h"
#include "account/Services.h"
#include "account/services/Conversions.h"
#include "account/services/ServiceContext.h"
#include "account/views/Coins.h"
#include "account/views/History.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "account/views/Settings.h"
#include "catalog/feedback/MessageModal.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "interfaces/signer.h"
#include "metatypes.h"
#include "modules/keys/KeysModule.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigModule.h"
#include "modules/musig/MusigServices.h"
#include "modules/musig/SavedPsbts.h"
#include "modules/musig/SettingsExtras.h"
#include "modules/musig/SigningSection.h"
#include "views/BackendErrors.h"
#include "views/utils.h"

#include <QDebug>
#include <QThread>
#include <common.h>
#include <memory>
#include <string>
#include <utility>

namespace {

const char *const NOT_LOADED = "Account not initialized";
const char *const NO_PSBT = "No transaction in flight";

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

// The widget owns the controller: deleting it on stopped() frees both.
MusigAccountController::MusigAccountController(const QString &account, MusigAccountWidget *widget)
    : AccountBackend(widget),
      m_widget(widget),
      // Before the account loads, so its services follow the load.
      m_services(new musig::MusigServices(account, this)) {
    init(account);
}

MusigAccountController::~MusigAccountController() {
    m_panels.clear();
}

void MusigAccountController::init(const QString &account) {
    if (m_init) {
        return;
    }

    m_init = true;
    connect(m_reconnect, &account::ElectrumReconnect::reconnect, this,
            &MusigAccountController::onElectrumReconnect, qontrol::UNIQUE);
    connect(m_reconnect, &account::ElectrumReconnect::gaveUp, this,
            &MusigAccountController::onElectrumGaveUp, qontrol::UNIQUE);
    connect(this, &AccountBackend::settingsIdle, this, &MusigAccountController::onSettingsIdle,
            qontrol::UNIQUE);

    // Notifications come from a blocking recv() on a dedicated thread.
    connect(this, &MusigAccountController::notificationReceived, this,
            &MusigAccountController::onNotificationReceived, qontrol::UNIQUE);
    m_loading_account = std::make_shared<std::optional<rust::Box<MusigAccount>>>();
    auto *worker = accountLoadWorker(account, m_loading_account, ::new_musig_account_offline);
    connect(worker, &QThread::finished, this, &MusigAccountController::onAccountLoaded,
            qontrol::UNIQUE);
    startWorker(worker);
}

void MusigAccountController::onAccountLoaded() {
    auto error = installLoadedConfig();
    m_account = std::move(*m_loading_account);
    m_loading_account = nullptr;
    if (m_account.has_value() && !m_account.value()->is_ok()) {
        auto failure = m_account.value()->get_error();
        error = QString::fromUtf8(failure.data(), failure.size());
    }
    if (endpointsStopping()) {
        finishAccountLoad(error);
        stop();
        return;
    }
    if (m_account.has_value() && m_account.value()->is_ok()) {
        setAccountDescriptor(m_account.value()->descriptor());
        listenNotifications();
    }
    // Loaded first: the account supplies its services from here on.
    finishAccountLoad(error);
    if ((m_widget != nullptr || m_panels_requested) && hasAccountMetadata()) {
        loadPanels();
        loadPanel(m_requested_panel);
        pollCoins();
    }
    if (!error.isEmpty()) {
        reportEndpointFailure(Endpoint::Electrum, rust::String(error.toStdString()),
                              BackendErrorKind::Other);
    } else if (m_widget != nullptr && !endpointOperationsBusy() && !electrumUrl().isEmpty()) {
        setEndpointConnected(0, Endpoint::Electrum, true);
    }
}

void MusigAccountController::listenNotifications() {
    auto receiver = m_account.value()->take_receiver();
    m_notif_thread =
        QThread::create([this, recv = std::move(receiver), generation = ++m_notif_generation]() {
            while (true) {
                auto poll = recv->recv();
                if (!poll->is_some()) {
                    break;
                }
                emit notificationReceived(generation, poll->get_notification());
            }
        });
    m_notif_thread->start();
}

void MusigAccountController::loadPanels() {
    m_panels_requested = true;
    if (!hasAccountMetadata() || !assembleServices()) {
        return;
    }
    auto services = account::servicesOf(*m_assembly);
    if (!m_panels.contains("settings")) {
        auto *panel = new qontrol::Panel(
            new account::view::Settings(services, settingsExtras(), true), "settings");
        m_panels.insert(panel);
    }
    if (m_panels.contains("history") || !m_account.has_value() || !m_account.value()->is_ok()) {
        return;
    }
    // The views open on the account's funds, not on an empty read.
    if (!m_services->fundsKnown()) {
        connect(m_services, &account::ServiceContext::fundsAnswered, this,
                &MusigAccountController::onFundsAnswered, qontrol::UNIQUE);
        return;
    }
    m_panels.insert(new qontrol::Panel(new account::view::History(services), "history"));
    m_panels.insert(new qontrol::Panel(new account::view::Coins(services), "coins"));
    m_panels.insert(new qontrol::Panel(new account::view::Receive(services, module()), "receive"));
    auto *send = new account::view::Send(services, new musig::SigningSection(this, services));
    m_panels.insert(new qontrol::Panel(send, "send"));
    connect(this, &MusigAccountController::savedPsbtWarningChanged, send,
            &account::view::Send::onSavedWarningChanged, qontrol::UNIQUE);
    m_panels.insert(new qontrol::Panel(new musig::SavedPsbts(this, services), "psbt"));
}

auto MusigAccountController::assembleServices() -> bool {
    if (m_assembly != nullptr) {
        return true;
    }
    // The account supplies its services once loaded.
    if (m_services_retired || accountLoading()) {
        return false;
    }
    auto result = account::assembleServices(module(), m_services->account(),
                                            account::services::toNetwork(network()));
    if (result.assembly == nullptr) {
        qWarning() << "MusigAccountController: account services refused:" << result.error;
        return false;
    }
    m_assembly = std::move(result.assembly);
    connect(m_assembly.get(), &host::ServiceAssembly::retired, this,
            &MusigAccountController::onServicesRetired, qontrol::UNIQUE);
    return true;
}

void MusigAccountController::onFundsAnswered() {
    loadPanels();
    loadPanel(m_requested_panel);
}

void MusigAccountController::onServicesRetired() {
    m_services_retired = true;
    m_panels.clear();
    // The assembly is the sender: it goes once its signal returned.
    m_assembly.release()->deleteLater();
}

auto MusigAccountController::screen(const QString &screen) -> std::optional<qontrol::Screen *> {
    return m_panels.screen(screen);
}

auto MusigAccountController::getAccount() -> std::optional<rust::Box<MusigAccount>> & {
    return m_account;
}

auto MusigAccountController::newAddress([[maybe_unused]] AddressKind kind) -> QString {
    if (m_account.has_value() && !accountDeletionPending() && !endpointsStopping()) {
        return toQString(m_account.value()->new_addr());
    }
    return {};
}

auto MusigAccountController::network() -> Network {
    return accountConfig().get_network();
}

auto MusigAccountController::getDescriptor() -> rust::String {
    return accountDescriptor();
}

auto MusigAccountController::cosignerFingerprints() -> QStringList {
    QStringList out;
    if (!hasAccountMetadata()) {
        return out;
    }
    for (const auto &fingerprint : ::musig_descriptor_fingerprints(getDescriptor())) {
        out.append(QString::fromStdString(std::string(fingerprint)));
    }
    return out;
}

auto MusigAccountController::endpoints() -> QList<Endpoint> {
    return {Endpoint::Electrum};
}

auto MusigAccountController::endpointUrl([[maybe_unused]] Endpoint endpoint) -> QString {
    return electrumUrl();
}

auto MusigAccountController::verifiesCertificate() const -> bool {
    return savedEndpointVerify();
}

auto MusigAccountController::isEndpointRunning(Endpoint endpoint) const -> bool {
    return endpoint == Endpoint::Electrum && m_electrum_state == ElectrumState::Connected;
}

auto MusigAccountController::endpointChangeRefusal() const -> QString {
    if (!m_sign_reqs.empty() || m_resetting || m_finalizing || m_broadcast_pending) {
        return QStringLiteral("Account is unavailable or signing is in progress");
    }
    return {};
}

auto MusigAccountController::electrumState() const -> ElectrumState {
    if (endpointsStopping()) {
        return ElectrumState::Disconnecting;
    }
    if (accountLoading()) {
        return ElectrumState::Connecting;
    }
    // A settings test or save changes no connection.
    if (endpointOperationsBusy() && !settingsBusy()) {
        return m_endpoint_connect ? ElectrumState::Connecting : ElectrumState::Disconnecting;
    }
    return m_electrum_state;
}

auto MusigAccountController::module() -> IModule * {
    return MusigModule::get();
}

auto MusigAccountController::settingsExtras() -> QWidget * {
    return new musig::SettingsExtras(this);
}

auto MusigAccountController::services() const -> account::ServiceContext * {
    return m_services;
}

void MusigAccountController::startWorker(QThread *worker) {
    if (m_stopping || endpointsStopping() || accountDeletionPending()) {
        worker->deleteLater();
        return;
    }
    startAcceptedWorker(worker);
}

void MusigAccountController::startAcceptedWorker(QThread *worker) {
    m_workers.append(worker);
    connect(worker, &QThread::finished, this, &MusigAccountController::onWorkerFinished,
            qontrol::UNIQUE);
    worker->start();
}

auto MusigAccountController::electrumUrl() -> QString {
    return savedEndpointUrl(Endpoint::Electrum);
}

auto MusigAccountController::roundStatus() const -> MusigRoundStatus {
    MusigRoundStatus status;
    status.is_ok = false;
    status.round = MusigRound::NotMusig;
    if (!m_account.has_value()) {
        status.error = rust::String(NOT_LOADED);
        return status;
    }
    if (m_psbt_v2_hex.isEmpty()) {
        status.error = rust::String(NO_PSBT);
        return status;
    }
    std::string psbt = m_psbt_v2_hex.toStdString();
    return m_account.value()->round_status(rust::Str(psbt));
}

auto MusigAccountController::psbtInFlight() const -> QString {
    return m_psbt_v2_hex;
}

auto MusigAccountController::finalizedTx() const -> QString {
    return m_tx_hex;
}

auto MusigAccountController::spendSigners() const -> QStringList {
    return m_spend_signers;
}

auto MusigAccountController::requestSigningPlan() -> uint64_t {
    auto reqId = ++m_next_plan_id;
    if (!m_account.has_value() || !m_account.value()->is_ok() || m_psbt_v2_hex.isEmpty() ||
        m_endpoint_busy || m_stopping || m_stop_requested) {
        return 0;
    }
    rust::Vec<LiveDevice> devices;
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.signer->state() == plugin::SignerState::Ready) {
            devices.push_back(keys::KeysModule::toLiveDevice(entry));
        }
    }
    auto plan = std::make_shared<SigningPlan>();
    auto *account = &*m_account.value();
    enqueueStorage(
        [account, plan, devices = std::move(devices)]() mutable {
            *plan = account->active_signing_plan(std::move(devices));
        },
        [this, plan, reqId] { emit signingPlanReady(reqId, *plan); });
    return reqId;
}

auto MusigAccountController::spendLeaves(const QStringList &outpoints) -> MusigSpendLeaves {
    if (!m_account.has_value()) {
        MusigSpendLeaves leaves;
        leaves.is_ok = false;
        leaves.error = rust::String(NOT_LOADED);
        return leaves;
    }
    rust::Vec<rust::String> list;
    for (const auto &outpoint : outpoints) {
        list.push_back(rust::String(outpoint.toStdString()));
    }
    return m_account.value()->spend_leaves(std::move(list));
}

void MusigAccountController::finalizeIfReady() {
    if (!m_account.has_value() || m_psbt_v2_hex.isEmpty() || m_finalizing || !m_tx_hex.isEmpty() ||
        m_restart_pending || m_resetting) {
        return;
    }
    m_finalizing = true;
    auto generation = m_signing_generation;
    auto result = std::make_shared<MusigAttemptSnapshot>();
    auto *account = &*m_account.value();
    enqueueStorage([account, result] { *result = account->finalize_active_attempt(); },
                   [this, result, generation] {
                       if (generation != m_signing_generation) {
                           return;
                       }
                       m_finalizing = false;
                       if (!result->is_ok) {
                           emit finalizeFailed(toQString(result->error));
                           return;
                       }
                       applyAttempt(*result);
                       if (!result->finalize_error.empty()) {
                           emit finalizeFailed(toQString(result->finalize_error));
                           return;
                       }
                       emit savedPsbtsChanged();
                       if (!m_tx_hex.isEmpty()) {
                           emit transactionReady(m_tx_hex);
                       }
                   });
}

// Serialize the durable archive, retirement and new attempt behind progress writes.
void MusigAccountController::restartRounds() {
    if (m_resetting || m_psbt_v2_hex.isEmpty() || m_finalizing || !m_sign_reqs.isEmpty() ||
        !m_tx_hex.isEmpty() || !m_account.has_value() || m_saved_record_id.isEmpty() ||
        m_stopping || m_stop_requested || m_endpoint_busy) {
        return;
    }
    m_resetting = true;
    m_restart_generation = ++m_signing_generation;
    m_restart_source = m_psbt_v2_hex;
    m_restart_result = std::make_shared<MusigAttemptSnapshot>();
    auto result = m_restart_result;
    auto *account = &*m_account.value();
    enqueueStorage([account, result] { *result = account->reset_active_attempt(); },
                   [this] { onRestartFinished(); });
    emit roundChanged(roundStatus());
}

auto MusigAccountController::isRestarting() const -> bool {
    return m_resetting;
}

auto MusigAccountController::restartPending() const -> bool {
    return m_restart_pending;
}

// ===== Slots =====

void MusigAccountController::loadPanel(const QString &name) {
    m_requested_panel = name;
    auto *panel = m_panels.value(name);
    if (m_widget != nullptr && panel != nullptr) {
        m_widget->loadPanel(panel);
    }
}

void MusigAccountController::pollCoins() {
    if (m_account.has_value()) {
        emit updateCoins(m_account.value()->spendable_coins());
        emit updateBalance(m_account.value()->balance());
        emit savedPsbtsChanged();
    }
}

void MusigAccountController::onNotificationReceived(uint64_t generation, Notification notif) {
    if (generation == m_notif_generation) {
        handleNotification(std::move(notif));
    }
}

void MusigAccountController::handleNotification(Notification notif) {
    if (m_endpoint_account != nullptr || m_stopping) {
        if (m_endpoint_account != nullptr && !endpointsStopping()) {
            m_endpoint_notifications.append(notif);
        }
        return;
    }
    switch (notif.flag) {
    case NotificationFlag::NewOutput:
    case NotificationFlag::OutputSpent:
    case NotificationFlag::CoinUpdate:
    case NotificationFlag::PaymentUpdated:
    case NotificationFlag::AddressTipChanged:
        pollCoins();
        refreshActiveWarning();
        break;
    case NotificationFlag::ElectrumStarted:
        m_electrum_state = ElectrumState::Connecting;
        emit endpointStateChanged(Endpoint::Electrum);
        break;
    case NotificationFlag::ElectrumConnected:
        m_electrum_state = ElectrumState::Connected;
        clearEndpointFailure(Endpoint::Electrum);
        m_reconnect->connected();
        finishEndpointConnection(Endpoint::Electrum, true, rust::String(), BackendErrorKind::None);
        emit electrumConnected(QString::fromStdString(std::string(notif.payload.c_str())));
        emit endpointStateChanged(Endpoint::Electrum);
        break;
    case NotificationFlag::ElectrumError:
        qWarning() << "musig: electrum:"
                   << QString::fromStdString(std::string(notif.payload.c_str()));
        m_electrum_state = ElectrumState::Disconnected;
        reportEndpointFailure(Endpoint::Electrum, notif.payload, ::notification_error_kind(notif));
        finishEndpointConnection(Endpoint::Electrum, false, notif.payload,
                                 ::notification_error_kind(notif));
        m_reconnect->lost();
        emit electrumDisconnected();
        emit endpointStateChanged(Endpoint::Electrum);
        break;
    case NotificationFlag::ElectrumStopped:
        m_electrum_state = ElectrumState::Disconnected;
        finishEndpointConnection(Endpoint::Electrum, false, rust::String("Electrum stopped"),
                                 BackendErrorKind::Electrum);
        m_reconnect->lost();
        emit electrumDisconnected();
        emit endpointStateChanged(Endpoint::Electrum);
        break;
    default:
        break;
    }
}

void MusigAccountController::onElectrumReconnect() {
    if (endpointsStopping()) {
        return;
    }
    if (endpointOperationsBusy()) {
        m_reconnect_pending = true;
    } else if (m_account.has_value()) {
        m_reconnect_pending = false;
        changeEndpoint(0, Endpoint::Electrum, true, true);
    }
}

void MusigAccountController::onSettingsIdle() {
    if (m_reconnect_pending && !endpointOperationsBusy()) {
        onElectrumReconnect();
    }
}

void MusigAccountController::onElectrumGaveUp(int losses) {
    auto server = electrumUrl();
    if (server.isEmpty()) {
        server = TR("electrum-the-server");
    }
    AppController::execModal(new catalog::MessageModal(
        TR("electrum-gave-up-title"), TR("electrum-gave-up-message").arg(server).arg(losses)));
}

void MusigAccountController::stop() {
    stopEndpointOperations();
    if (m_endpoint_busy || accountLoading()) {
        m_stop_requested = true;
        return;
    }
    if (m_stopping) {
        return;
    }
    // Flush queued progress before the account is dropped.
    if (!m_storage_queue.empty()) {
        m_stop_requested = true;
        cancelSigning();
        return;
    }
    m_stopping = true;
    ++m_notif_generation;
    m_electrum_state = ElectrumState::Disconnected;
    emit endpointStateChanged(Endpoint::Electrum);
    cancelSigning();
    m_reconnect->stopped();

    if (!m_account.has_value() && m_workers.isEmpty()) {
        onShutdownDrained({});
        return;
    }

    // The account and the threads go to a background thread so the GUI never blocks:
    // dropping the account closes the channel, which ends the listener's recv();
    // stopped() fires once the listener has exited. Each thread is deleted only after
    // the wait, so none can be gone before the teardown waits on it.
    auto account = std::move(m_account);
    m_account.reset();
    QThread *notif = m_notif_thread;
    m_notif_thread = nullptr;
    // A running broadcast or worker uses the account or a view: they end first.
    auto workers = std::move(m_workers);
    m_workers.clear();

    drainAccount(std::move(account), notif, workers);
}

void MusigAccountController::adoptTransaction(const backend::TransactionTemplate &tx,
                                              const MusigSpendLeaf &spend, const QByteArray &psbt) {
    if (!m_account.has_value() || m_endpoint_busy || endpointsStopping() ||
        accountDeletionPending()) {
        emit prepareFailed(TR("send-no-account-loaded"));
        return;
    }
    cancelSigning();
    emit sessionLoading();
    auto generation = m_signing_generation;
    auto result = std::make_shared<std::optional<rust::Box<PsbtResult>>>();
    auto snapshot = std::make_shared<MusigAttemptSnapshot>();
    auto *account = &*m_account.value();
    enqueueStorage(
        [account, result, snapshot, tx = account::services::toTemplate(tx), spend, psbt] {
            result->emplace(
                account->adopt_transaction(tx, spend, account::services::toSlice(psbt)));
            *snapshot = account->active_attempt();
        },
        [this, result, snapshot, generation] {
            emit savedPsbtsChanged();
            if (generation != m_signing_generation) {
                return;
            }
            auto &psbt = result->value();
            if (!psbt->is_ok()) {
                // A reused change index says to craft again.
                emit prepareFailed(mapBackendErrorSummary(psbt->get_error_kind()) +
                                   QStringLiteral("\n\n") +
                                   formatBackendErrorDetails(toQString(psbt->get_psbt_error())));
                return;
            }
            applyAttempt(*snapshot);
            emit prepared(m_psbt_v2_hex, toQString(psbt->get_txid_preview()));
            emit roundChanged(roundStatus());
        });
}

auto MusigAccountController::beginSignRequest(const QString &fingerprint)
    -> std::pair<IKeys *, ReqId> {
    if (m_restart_pending || m_endpoint_busy) {
        return {nullptr, 0};
    }
    if (m_resetting) {
        return {nullptr, 0};
    }
    if (m_psbt_v2_hex.isEmpty() || !m_account.has_value() ||
        signingKeys().contains(fingerprint.toLower())) {
        return {nullptr, 0};
    }
    auto *keys = Host::get()->keys(MusigModule::get());
    if (keys == nullptr) {
        emit signingFailed(TR("musig-send-no-keys"), plugin::SignError::Other);
        return {nullptr, 0};
    }
    connect(keys, &IKeys::signed_, this, &MusigAccountController::onKeysSigned, qontrol::UNIQUE);
    connect(keys, &IKeys::error, this, &MusigAccountController::onKeysError, qontrol::UNIQUE);
    connect(keys, &IKeys::cancelled, this, &MusigAccountController::onKeysCancelled,
            qontrol::UNIQUE);
    // The answer may arrive during the request: the id is stored first.
    auto req = keys->newRequest();
    m_signing_keys = keys;
    m_sign_reqs.insert(req, fingerprint);
    return {keys, req};
}

auto MusigAccountController::signingKeys() const -> QSet<QString> {
    QSet<QString> keys;
    for (const auto &fingerprint : m_sign_reqs) {
        keys.insert(fingerprint.toLower());
    }
    return keys;
}

void MusigAccountController::signRound(const QString &fingerprint) {
    auto [keys, req] = beginSignRequest(fingerprint);
    if (keys == nullptr) {
        return;
    }
    keys->sign(req, account::toPublic<plugin::Network>(network()), m_psbt_v2_hex,
               QStringList{toQString(getDescriptor())}, toQString(m_account.value()->name()),
               fingerprint);
}

void MusigAccountController::signRoundOnDevice(const QString &fingerprint) {
    auto [keys, req] = beginSignRequest(fingerprint);
    if (keys == nullptr) {
        return;
    }
    keys->signOnDevice(req, account::toPublic<plugin::Network>(network()), m_psbt_v2_hex,
                       toQString(getDescriptor()), fingerprint);
}

void MusigAccountController::prepareNonces(const QString &fingerprint) {
    if (m_restart_pending || m_endpoint_busy || m_resetting || m_psbt_v2_hex.isEmpty() ||
        !m_account.has_value() || !m_prepare_reqs.isEmpty() ||
        preparingKeys().contains(fingerprint.toLower()) ||
        signingKeys().contains(fingerprint.toLower())) {
        return;
    }
    auto *keys = Host::get()->keys(MusigModule::get());
    if (keys == nullptr) {
        emit signingFailed(TR("musig-send-no-keys"), plugin::SignError::Other);
        return;
    }
    connect(keys, &IKeys::noncesPrepared, this, &MusigAccountController::onNoncesPrepared,
            qontrol::UNIQUE);
    connect(keys, &IKeys::error, this, &MusigAccountController::onKeysError, qontrol::UNIQUE);
    connect(keys, &IKeys::cancelled, this, &MusigAccountController::onKeysCancelled,
            qontrol::UNIQUE);
    auto req = keys->newRequest();
    m_signing_keys = keys;
    m_prepare_reqs.insert(req, fingerprint);
    keys->prepareNonces(req, account::toPublic<plugin::Network>(network()), m_psbt_v2_hex,
                        toQString(getDescriptor()), fingerprint);
}

auto MusigAccountController::preparingKeys() const -> QSet<QString> {
    QSet<QString> keys;
    for (const auto &fingerprint : m_prepare_reqs) {
        keys.insert(fingerprint.toLower());
    }
    return keys;
}

auto MusigAccountController::noncesAllocated() const -> bool {
    return m_nonces_allocated;
}

void MusigAccountController::retireSignRequests() {
    auto requests = m_sign_reqs.keys();
    m_sign_reqs.clear();
    m_accepting_reqs.clear();
    requests += m_prepare_reqs.keys();
    m_prepare_reqs.clear();
    if (!m_signing_keys.isNull()) {
        for (auto req : requests) {
            m_signing_keys->cancelRequest(req);
        }
    }
}

void MusigAccountController::clearSigning() {
    ++m_signing_generation;
    m_restart_pending = false;
    m_spend_signers.clear();
    m_psbt_v2_hex.clear();
    m_tx_hex.clear();
    m_saved_record_id.clear();
    m_finalizing = false;
    m_nonces_allocated = false;
    retireSignRequests();
    if (m_account.has_value()) {
        auto *account = &*m_account.value();
        enqueueStorage(
            [account] {
                if (!account->clear_active_attempt()) {
                    qWarning() << "Failed to clear active signing attempt";
                }
            },
            [] {});
    }
}

void MusigAccountController::cancelSigning() {
    clearSigning();
    emit sessionCleared();
}

auto MusigAccountController::startBroadcast(
    const QByteArray &transaction, std::function<void(const backend::BroadcastResult &)> done)
    -> bool {
    // The storage queue drops work once the controller stops.
    if (m_restart_pending || m_resetting || !m_account.has_value() || m_broadcast_pending ||
        m_endpoint_busy || m_stopping || m_stop_requested || accountDeletionPending()) {
        return false;
    }
    // stop() waits on this thread before it drops the account and before stopped()
    // lets the owner delete this controller, so both outlive the worker.
    auto *account = &*m_account.value();
    auto generation = m_signing_generation;
    auto inFlight = !m_tx_hex.isEmpty() && transaction == QByteArray::fromHex(m_tx_hex.toLatin1());
    auto result = std::make_shared<backend::BroadcastResult>();
    auto warning = std::make_shared<QString>();
    m_broadcast_pending = true;
    enqueueStorage(
        [account, result, warning, transaction] {
            *result = account::services::toBroadcastResult(
                account->broadcast_report(account::services::toSlice(transaction)));
            *warning = toQString(account->saved_psbt_error());
        },
        [this, result, warning, generation, inFlight, done = std::move(done)] {
            m_broadcast_pending = false;
            setSaveWarning(*warning);
            emit savedPsbtsChanged();
            if (inFlight && generation == m_signing_generation &&
                result->outcome == backend::BroadcastOutcome::Accepted) {
                cancelSigning();
            }
            pollCoins();
            done(*result);
        });
    return true;
}

void MusigAccountController::onKeysSigned(ReqId req_id, const QString &psbt_v2_hex) {
    if (!m_sign_reqs.contains(req_id) || m_accepting_reqs.contains(req_id)) {
        return;
    }
    auto fingerprint = m_sign_reqs.value(req_id);
    m_accepting_reqs.insert(req_id);
    auto *account = &*m_account.value();
    auto generation = m_signing_generation;
    auto result = std::make_shared<MusigAttemptSnapshot>();
    enqueueStorage(
        [account, result, answer = psbt_v2_hex.toStdString()] {
            *result = account->accept_signed_progress(answer);
        },
        [this, result, fingerprint, generation, req_id] {
            emit savedPsbtsChanged();
            if (generation != m_signing_generation) {
                return;
            }
            m_sign_reqs.remove(req_id);
            m_accepting_reqs.remove(req_id);
            if (!result->is_ok) {
                emit signingFailed(toQString(result->error), plugin::SignError::Other);
                return;
            }
            applyAttempt(*result);
            emit signerAnswered(fingerprint);
            emit roundChanged(roundStatus());
            if (!result->finalize_error.empty()) {
                emit finalizeFailed(toQString(result->finalize_error));
            }
            if (!m_tx_hex.isEmpty()) {
                retireSignRequests();
                emit transactionReady(m_tx_hex);
            }
        });
}

void MusigAccountController::onKeysError(std::optional<ReqId> req_id, const QString &message,
                                         plugin::SignError kind) {
    if (!req_id.has_value()) {
        return;
    }
    if (m_prepare_reqs.contains(req_id.value())) {
        m_prepare_reqs.remove(req_id.value());
        emit signingFailed(message, kind);
        return;
    }
    if (!m_sign_reqs.contains(req_id.value())) {
        return;
    }
    m_sign_reqs.remove(req_id.value());
    if (kind == plugin::SignError::MusigSession) {
        // Pending answers belong to a session that cannot safely resume.
        retireSignRequests();
        m_restart_pending = true;
        auto *account = &*m_account.value();
        auto generation = m_signing_generation;
        auto snapshot = std::make_shared<MusigAttemptSnapshot>();
        enqueueStorage([account, snapshot] { *snapshot = account->require_active_restart(); },
                       [this, snapshot, generation] {
                           if (generation == m_signing_generation && snapshot->is_ok) {
                               applyAttempt(*snapshot);
                               emit roundChanged(roundStatus());
                           }
                       });
        emit signingFailed(message, kind);
        emit roundChanged(roundStatus());
        return;
    }
    // The request may still have taken sessions before it failed: Reset follows the
    // durable allocation, not the request's outcome.
    refreshAttempt();
    emit signingFailed(message, kind);
}

// Re-reads the active attempt from Rust and refreshes everything that follows it.
void MusigAccountController::refreshAttempt() {
    if (!m_account.has_value()) {
        return;
    }
    auto *account = &*m_account.value();
    auto generation = m_signing_generation;
    auto snapshot = std::make_shared<MusigAttemptSnapshot>();
    enqueueStorage([account, snapshot] { *snapshot = account->active_attempt(); },
                   [this, snapshot, generation] {
                       if (generation == m_signing_generation) {
                           applyAttempt(*snapshot);
                           emit roundChanged(roundStatus());
                       }
                   });
}

void MusigAccountController::onRestartFinished() {
    m_resetting = false;
    if (m_stopping) {
        return;
    }
    if (m_restart_generation != m_signing_generation || m_restart_source != m_psbt_v2_hex) {
        if (!m_psbt_v2_hex.isEmpty()) {
            emit roundChanged(roundStatus());
        }
        return;
    }
    if (!m_restart_result->is_ok) {
        m_restart_pending = m_restart_pending || m_restart_result->reset_pending;
        setSaveWarning(toQString(m_restart_result->save_error));
        emit signingFailed(toQString(m_restart_result->error), plugin::SignError::Other);
        emit roundChanged(roundStatus());
        return;
    }
    applyAttempt(*m_restart_result);
    emit savedPsbtsChanged();
    emit signingReset();
    emit roundChanged(roundStatus());
}

// The PSBT keeps the round it was at.
void MusigAccountController::onKeysCancelled(ReqId req_id) {
    if (m_prepare_reqs.contains(req_id)) {
        m_prepare_reqs.remove(req_id);
        emit signingCancelled();
        return;
    }
    if (!m_sign_reqs.contains(req_id)) {
        return;
    }
    m_sign_reqs.remove(req_id);
    refreshAttempt();
    emit signingCancelled();
}

void MusigAccountController::onNoncesPrepared(ReqId req_id,
                                              [[maybe_unused]] const QString &fingerprint) {
    if (!m_prepare_reqs.contains(req_id)) {
        return;
    }
    m_prepare_reqs.remove(req_id);
    emit roundChanged(roundStatus());
}

// Once stop() took the workers, its teardown deletes them.
void MusigAccountController::onWorkerFinished() {
    for (auto it = m_workers.begin(); it != m_workers.end();) {
        if ((*it)->isFinished()) {
            (*it)->deleteLater();
            it = m_workers.erase(it);
        } else {
            ++it;
        }
    }
}

void MusigAccountController::historyClicked() {
    loadPanel("history");
}

void MusigAccountController::coinsClicked() {
    loadPanel("coins");
}

void MusigAccountController::sendClicked() {
    loadPanel("send");
}

void MusigAccountController::receiveClicked() {
    loadPanel("receive");
}

void MusigAccountController::settingsClicked() {
    loadPanel("settings");
}

auto MusigAccountController::saveWarning() const -> QString {
    return m_save_warning;
}

auto MusigAccountController::activeRecordId() const -> QString {
    return m_saved_record_id;
}

void MusigAccountController::setSaveWarning(const QString &message) {
    m_save_warning = message;
    emit saveWarningChanged(message);
}

void MusigAccountController::enqueueStorage(std::function<void()> work,
                                            std::function<void()> done) {
    if (m_stopping || m_stop_requested || accountDeletionPending() || !m_account.has_value()) {
        return;
    }
    m_storage_queue.push_back({std::move(work), std::move(done)});
    startStorage();
}

void MusigAccountController::startStorage() {
    if (m_storage_thread != nullptr || m_storage_queue.empty() || m_stopping) {
        return;
    }
    m_storage_thread = QThread::create(m_storage_queue.front().work);
    connect(m_storage_thread, &QThread::finished, this, &MusigAccountController::onStorageFinished,
            qontrol::UNIQUE);
    startAcceptedWorker(m_storage_thread);
}

void MusigAccountController::onStorageFinished() {
    m_storage_thread = nullptr;
    if (m_stopping) {
        return;
    }
    auto done = std::move(m_storage_queue.front().done);
    m_storage_queue.pop_front();
    done();
    startStorage();
    if (m_endpoint_busy && m_storage_queue.empty()) {
        startEndpointChange();
    } else if (m_stop_requested && m_storage_queue.empty()) {
        stop();
    }
}

void MusigAccountController::requestTxDetails(uint64_t req_id, const QString &txid) {
    requestAccountTxDetails(req_id, txid, m_account.has_value() ? &*m_account.value() : nullptr,
                            isEndpointRunning(Endpoint::Electrum) && !m_endpoint_busy);
}

void MusigAccountController::requestEndpointTip(uint64_t req_id, Endpoint endpoint) {
    if (endpoint != Endpoint::Electrum || !m_account.has_value() || endpointOperationsBusy() ||
        !isEndpointRunning(endpoint)) {
        ElectrumTipResult result;
        result.is_ok = false;
        result.error = rust::String("Endpoint is disconnected or unsupported");
        emit endpointTipReady(req_id, endpoint, result);
        return;
    }
    requestAccountEndpointTip(req_id, endpoint);
}

void MusigAccountController::setEndpointConnected(uint64_t req_id, Endpoint endpoint,
                                                  bool connected) {
    changeEndpoint(req_id, endpoint, connected);
}

void MusigAccountController::changeEndpoint(uint64_t req_id, Endpoint endpoint, bool connected,
                                            bool retry) {
    if (endpoint != Endpoint::Electrum || endpointOperationsBusy() || m_stopping ||
        m_stop_requested || !m_account.has_value() || !endpointChangeRefusal().isEmpty()) {
        EndpointResult result{};
        result.kind = BackendErrorKind::Other;
        result.is_ok = false;
        result.error = rust::String("Account is unavailable or signing is in progress");
        if (endpointsStopping()) {
            result.error = rust::String("Account is stopping");
        } else if (endpointOperationsBusy()) {
            result.error = rust::String("Endpoint operation is in progress");
        }
        emit endpointRequestFinished(req_id, endpoint, result);
        return;
    }
    m_endpoint_busy = true;
    m_endpoint_retry = retry;
    m_endpoint_notifications.clear();
    m_endpoint = endpoint;
    m_endpoint_connect = connected;
    if (!connected) {
        m_reconnect->stopped();
        m_reconnect_pending = false;
    }
    beginEndpointOperation(req_id, endpoint,
                           connected ? EndpointOperation::Connecting
                                     : EndpointOperation::Disconnecting);
    if (m_storage_queue.empty()) {
        startEndpointChange();
    }
}

void MusigAccountController::startEndpointChange() {
    m_endpoint_account =
        std::make_shared<std::optional<rust::Box<MusigAccount>>>(std::move(m_account));
    m_account.reset();
    m_endpoint_result = std::make_shared<EndpointResult>();
    m_endpoint_replaced = std::make_shared<bool>(false);
    auto workers = std::move(m_workers);
    m_workers.clear();
    auto *thread = endpointChangeWorker(
        m_endpoint_account, m_endpoint_result, m_endpoint_replaced, workers, m_notif_thread,
        [connected = m_endpoint_connect](rust::Box<MusigAccount> account) {
            return ::musig_endpoint_change(std::move(account), connected);
        },
        ::take_musig_endpoint_account);
    connect(thread, &QThread::finished, this, &MusigAccountController::onEndpointFinished,
            qontrol::UNIQUE);
    startWorker(thread);
}

void MusigAccountController::onEndpointFinished() {
    m_account = std::move(*m_endpoint_account);
    m_endpoint_account = nullptr;
    m_endpoint_busy = false;
    if (m_account.has_value() && m_account.value()->is_ok()) {
        setAccountDescriptor(m_account.value()->descriptor());
        setSavedEndpoint(Endpoint::Electrum, m_account.value()->electrum_url(),
                         m_account.value()->verify_certificate());
        if (m_panels_requested) {
            loadPanels();
        }
    }
    if (*m_endpoint_replaced) {
        if (!m_endpoint_retry) {
            m_reconnect->stopped();
            m_reconnect_pending = false;
        }
        m_notif_thread = nullptr;
        m_electrum_state = m_endpoint_result->connection_pending ? ElectrumState::Connecting
                                                                 : ElectrumState::Disconnected;
        if (m_account.has_value() && m_account.value()->is_ok()) {
            listenNotifications();
        }
        emit electrumDisconnected();
        emit electrumUrlChanged();
    }
    emit endpointStateChanged(Endpoint::Electrum);
    auto notifications = std::move(m_endpoint_notifications);
    m_endpoint_notifications.clear();
    bool replaced = *m_endpoint_replaced;
    bool retry = m_endpoint_retry;
    auto result = *m_endpoint_result;
    finishEndpointOperation(result);
    if (retry && !result.is_ok && !endpointsStopping()) {
        m_reconnect->lost();
    }
    if (!replaced && !endpointsStopping()) {
        for (const auto &notification : notifications) {
            handleNotification(notification);
        }
    }
    if (!m_psbt_v2_hex.isEmpty()) {
        emit roundChanged(roundStatus());
    }
    if (m_stop_requested) {
        stop();
    } else if (m_reconnect_pending && !endpointOperationsBusy()) {
        onElectrumReconnect();
    }
}

void MusigAccountController::applyAttempt(const MusigAttemptSnapshot &snapshot) {
    m_psbt_v2_hex = toQString(snapshot.psbt_v2_hex);
    m_tx_hex = toQString(snapshot.tx_hex);
    m_saved_record_id = toQString(snapshot.record_id);
    m_restart_pending = snapshot.reset_pending;
    m_nonces_allocated = snapshot.nonces_allocated;
    m_spend_signers.clear();
    for (const auto &fingerprint : snapshot.signers) {
        m_spend_signers.append(toQString(fingerprint));
    }
    setSaveWarning(toQString(snapshot.save_error));
}

void MusigAccountController::listSavedPsbts(bool include_confirmed) {
    if (!m_account.has_value()) {
        return;
    }
    auto *account = &*m_account.value();
    auto result = std::make_shared<SavedPsbtList>();
    enqueueStorage(
        [account, result, include_confirmed] { *result = account->saved_psbts(include_confirmed); },
        [this, result] {
            setSaveWarning(toQString(result->save_error));
            emit savedPsbtsListed(*result);
        });
}

void MusigAccountController::restoreSavedPsbt(const QString &id) {
    if (!m_account.has_value()) {
        return;
    }
    clearSigning();
    emit sessionLoading();
    auto generation = m_signing_generation;
    auto *account = &*m_account.value();
    auto result = std::make_shared<SavedPsbtRestore>();
    auto snapshot = std::make_shared<MusigAttemptSnapshot>();
    enqueueStorage(
        [account, result, snapshot, id = id.toStdString()] {
            *result = account->activate_saved_psbt(id);
            *snapshot = account->active_attempt();
        },
        [this, result, snapshot, generation] {
            if (generation != m_signing_generation) {
                return;
            }
            if (!result->is_ok) {
                emit sessionCleared();
                emit savedPsbtFailed(toQString(result->error));
                return;
            }
            auto &entry = result->entry;
            applyAttempt(*snapshot);
            emit savedPsbtRestored(entry);
            loadPanel("send");
            auto status = roundStatus();
            emit roundChanged(status);
            if (!snapshot->finalize_error.empty()) {
                emit finalizeFailed(toQString(snapshot->finalize_error));
            }
        });
}

void MusigAccountController::deleteSavedPsbt(const QString &id) {
    if (!m_account.has_value() || m_deletions.contains(id)) {
        return;
    }
    m_deletions.insert(id, DeletionState::Checking);
    auto *account = &*m_account.value();
    auto fresh = std::make_shared<SavedPsbtRestore>();
    auto deleted = std::make_shared<SavedPsbtMutation>();
    enqueueStorage(
        [account, fresh, deleted, id = id.toStdString()] {
            *fresh = account->restore_saved_psbt(id);
            // No GUI round trip between the chain-state check and prompt-free deletion.
            if (fresh->is_ok && fresh->entry.confirmed_height != 0) {
                *deleted = account->delete_saved_psbt(id);
            }
        },
        [this, fresh, deleted, id] {
            if (!fresh->is_ok) {
                m_deletions.remove(id);
                emit savedPsbtFailed(toQString(fresh->error));
                return;
            }
            if (fresh->entry.confirmed_height == 0) {
                m_deletions[id] = DeletionState::AwaitingConfirmation;
                emit savedPsbtDeleteConfirmationNeeded(id);
                return;
            }
            completeSavedPsbtDeletion(id, *deleted);
        });
}

void MusigAccountController::onSavedPsbtDeleteConfirmed(const QString &id) {
    auto pending = m_deletions.constFind(id);
    if (!m_account.has_value() || pending == m_deletions.cend() ||
        pending.value() != DeletionState::AwaitingConfirmation) {
        return;
    }
    m_deletions[id] = DeletionState::Deleting;
    auto *account = &*m_account.value();
    auto result = std::make_shared<SavedPsbtMutation>();
    enqueueStorage(
        [account, result, id = id.toStdString()] { *result = account->delete_saved_psbt(id); },
        [this, result, id] { completeSavedPsbtDeletion(id, *result); });
}

void MusigAccountController::onSavedPsbtDeleteCancelled(const QString &id) {
    auto pending = m_deletions.constFind(id);
    if (pending != m_deletions.cend() && pending.value() == DeletionState::AwaitingConfirmation) {
        m_deletions.remove(id);
    }
}

void MusigAccountController::completeSavedPsbtDeletion(const QString &id,
                                                       const SavedPsbtMutation &result) {
    m_deletions.remove(id);
    if (!result.is_ok) {
        emit savedPsbtFailed(toQString(result.error));
    } else if (id == m_saved_record_id || m_saved_record_id.isEmpty()) {
        cancelSigning();
    }
    setSaveWarning(toQString(result.save_error));
    emit savedPsbtsChanged();
}

void MusigAccountController::labelSavedPsbt(const QString &id, const QString &label) {
    if (!m_account.has_value()) {
        return;
    }
    auto *account = &*m_account.value();
    auto result = std::make_shared<SavedPsbtMutation>();
    enqueueStorage(
        [account, result, id = id.toStdString(), label = label.toStdString()] {
            *result = account->label_saved_psbt(id, rust::String(label));
        },
        [this, result] {
            if (!result->is_ok) {
                emit savedPsbtFailed(toQString(result->error));
            }
            setSaveWarning(toQString(result->save_error));
            emit savedPsbtsChanged();
        });
}

void MusigAccountController::retrySavedPsbts() {
    if (!m_account.has_value()) {
        return;
    }
    auto *account = &*m_account.value();
    auto result = std::make_shared<SavedPsbtMutation>();
    enqueueStorage([account, result] { *result = account->retry_saved_psbts(); },
                   [this, result] {
                       setSaveWarning(
                           toQString(result->is_ok ? result->save_error : result->error));
                       emit savedPsbtsChanged();
                   });
}

void MusigAccountController::psbtClicked() {
    loadPanel("psbt");
}

void MusigAccountController::refreshActiveWarning() {
    if (!m_account.has_value() || m_saved_record_id.isEmpty()) {
        return;
    }
    auto *account = &*m_account.value();
    auto generation = m_signing_generation;
    auto result = std::make_shared<SavedPsbtRestore>();
    enqueueStorage(
        [account, result, id = m_saved_record_id.toStdString()] {
            *result = account->restore_saved_psbt(id);
        },
        [this, result, generation] {
            if (generation == m_signing_generation && result->is_ok) {
                emit savedPsbtWarningChanged(toQString(result->entry.warning));
            }
        });
}
