#include "AccountController.h"
#include "AccountWidget.h"
#include "AppController.h"
#include "SpModule.h"
#include "SpSigning.h"
#include "account/Services.h"
#include "account/services/AccountServices.h"
#include "account/services/ServiceContext.h"
#include "account/views/Coins.h"
#include "account/views/History.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "account/views/Settings.h"
#include "catalog/feedback/MessageModal.h"
#include "i18n/Tr.h"
#include "metatypes.h"
#include <Qontrol>
#include <common.h>
#include <qlogging.h>
#include <string>
#include <utility>

// The widget owns the controller: deleting it on stopped() frees both.
AccountController::AccountController(const QString &account, AccountWidget *widget)
    : AccountBackend(widget) {
    m_widget = widget;
    // Before the account loads, so its services follow the load.
    m_services = new account::AccountServices<AccountController, Account>(account, this);
    init(account);
}

AccountController::AccountController(AccountWidget *widget) : AccountBackend(widget) {
    m_widget = widget;
}

auto AccountController::init(const QString &account) -> void {
    if (m_init)
        return;

    m_init = true;
    connect(m_reconnect, &account::ElectrumReconnect::reconnect, this,
            &AccountController::onElectrumReconnect, qontrol::UNIQUE);
    connect(m_reconnect, &account::ElectrumReconnect::gaveUp, this,
            &AccountController::onElectrumGaveUp, qontrol::UNIQUE);
    connect(this, &AccountBackend::settingsIdle, this, &AccountController::onSettingsIdle,
            qontrol::UNIQUE);

    // Take the notification receiver and spawn a blocking listener thread
    connect(this, &AccountController::notificationReceived, this,
            &AccountController::onNotificationReceived, qontrol::UNIQUE);
    m_loading_account = std::make_shared<std::optional<rust::Box<Account>>>();
    auto *worker = accountLoadWorker(account, m_loading_account, ::new_account_offline);
    connect(worker, &QThread::finished, this, &AccountController::onAccountLoaded, qontrol::UNIQUE);
    startWorker(worker);
}

void AccountController::onAccountLoaded() {
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
        m_electrum_expected_count = static_cast<int>(m_account.value()->sub_account_count());
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
        for (auto endpoint : endpoints()) {
            reportEndpointFailure(endpoint, rust::String(error.toStdString()),
                                  BackendErrorKind::Other);
        }
    } else if (m_widget != nullptr && !endpointOperationsBusy()) {
        // Electrum connects at startup only for the sub-accounts, as the Rust startup does.
        if (m_electrum_expected_count > 0 && !endpointUrl(Endpoint::Electrum).isEmpty()) {
            probeElectrum(false);
        } else {
            changeEndpoint(0, Endpoint::Electrum, true, false, true);
        }
    }
}

AccountController::~AccountController() {
    m_panels.clear();
}

auto AccountController::assembleServices() -> bool {
    if (m_assembly != nullptr) {
        return true;
    }
    // The account supplies its services once loaded.
    if (m_services_retired || m_services == nullptr || accountLoading()) {
        return false;
    }
    auto result = account::assembleServices(module(), m_services->account(),
                                            account::services::toNetwork(network()));
    if (result.assembly == nullptr) {
        qWarning() << "AccountController: account services refused:" << result.error;
        return false;
    }
    m_assembly = std::move(result.assembly);
    connect(m_assembly.get(), &host::ServiceAssembly::retired, this,
            &AccountController::onServicesRetired, qontrol::UNIQUE);
    return true;
}

void AccountController::onFundsAnswered() {
    loadPanels();
    loadPanel(m_requested_panel);
}

void AccountController::onServicesRetired() {
    m_services_retired = true;
    m_panels.clear();
    // The assembly is the sender: it goes once its signal returned.
    m_assembly.release()->deleteLater();
}

void AccountController::listenNotifications() {
    auto receiver = m_account.value()->take_receiver();
    m_notif_thread =
        QThread::create([this, recv = std::move(receiver), generation = ++m_notif_generation]() {
            while (true) {
                auto poll = recv->recv();
                if (!poll->is_some()) {
                    break;
                }
                auto notif = poll->get_notification();
                emit notificationReceived(generation, notif);
            }
        });
    m_notif_thread->start();
}

auto AccountController::loadPanel(const QString &name) -> void {
    m_requested_panel = name;
    auto *panel = m_panels.value(name);
    if (m_widget != nullptr && panel != nullptr) {
        m_widget->loadPanel(panel);
    }
}

auto AccountController::insertPanel(qontrol::Panel *panel) -> void {
    m_panels.insert(panel);
}

auto AccountController::screen(const QString &screen) -> std::optional<qontrol::Screen *> {
    return m_panels.screen(screen);
}

auto AccountController::pollCoins() -> void {
    if (m_account.has_value()) {
        auto coinState = m_account.value()->spendable_coins();
        emit updateCoins(coinState);

        auto balance = m_account.value()->balance();
        emit updateBalance(balance);
    }
}

auto AccountController::etaSecs() const -> uint64_t {
    return m_estimator->estimate_secs();
}

void AccountController::onNotificationReceived(uint64_t generation, Notification notif) {
    if (generation == m_notif_generation) {
        handleNotification(std::move(notif));
    }
}

auto AccountController::handleNotification(Notification notif) -> void {
    if (m_endpoint_busy || m_stopping) {
        if (m_endpoint_busy && !endpointsStopping()) {
            m_endpoint_notifications.append(notif);
        }
        return;
    }
    auto flag = notif.flag;

    switch (flag) {
    case NotificationFlag::ScanProgress: {
        m_estimator->update(notif.current, notif.end);
        emit scanProgress(notif.current, notif.end);
        break;
    }
    case NotificationFlag::NewOutput:
    case NotificationFlag::OutputSpent:
        pollCoins();
        break;
    case NotificationFlag::FailStartScanning:
        m_scanner_running = false;
        reportEndpointFailure(Endpoint::Blindbit, notif.payload, ::notification_error_kind(notif));
        finishEndpointConnection(Endpoint::Blindbit, false, notif.payload,
                                 ::notification_error_kind(notif));
        emit endpointStateChanged(Endpoint::Blindbit);
        break;
    case NotificationFlag::FailScan:
        reportEndpointFailure(Endpoint::Blindbit, notif.payload, ::notification_error_kind(notif));
        finishEndpointConnection(Endpoint::Blindbit, false, notif.payload,
                                 ::notification_error_kind(notif));
        break;
    case NotificationFlag::StartingScan:
        m_estimator->reset();
        break;
    case NotificationFlag::ScanStarted:
        m_scanner_running = true;
        clearEndpointFailure(Endpoint::Blindbit);
        finishEndpointConnection(Endpoint::Blindbit, true, rust::String(), BackendErrorKind::None);
        emit scannerStateChanged(true);
        emit endpointStateChanged(Endpoint::Blindbit);
        break;
    case NotificationFlag::StoppingScan:
        break;
    case NotificationFlag::ScanCompleted:
        break;
    case NotificationFlag::ScanStopped:
        m_scanner_running = false;
        finishEndpointConnection(Endpoint::Blindbit, false, rust::String("Scanner stopped"),
                                 BackendErrorKind::Backend);
        m_estimator->reset();
        emit scannerStateChanged(false);
        emit endpointStateChanged(Endpoint::Blindbit);
        break;
    case NotificationFlag::WaitingForBlocks: {
        m_scanner_running = true;
        clearEndpointFailure(Endpoint::Blindbit);
        finishEndpointConnection(Endpoint::Blindbit, true, rust::String(), BackendErrorKind::None);
        emit scannerStateChanged(true);
        emit endpointStateChanged(Endpoint::Blindbit);
        emit waitingForBlocks(notif.current);
        break;
    }
    case NotificationFlag::NewBlocksDetected:
        break;
    case NotificationFlag::CoinUpdate:
    case NotificationFlag::AddressTipChanged:
        pollCoins();
        break;
    case NotificationFlag::ElectrumStarted:
        break;
    case NotificationFlag::ElectrumConnected: {
        m_electrum_connected_count++;
        if (m_electrum_connected_count >= m_electrum_expected_count) {
            auto payload = QString::fromStdString(std::string(notif.payload.c_str()));
            m_electrum_running = true;
            clearEndpointFailure(Endpoint::Electrum);
            m_reconnect->connected();
            finishEndpointConnection(Endpoint::Electrum, true, rust::String(),
                                     BackendErrorKind::None);
            emit electrumConnected(payload);
            emit endpointStateChanged(Endpoint::Electrum);
        }
        break;
    }
    case NotificationFlag::ElectrumError:
        m_electrum_running = false;
        reportEndpointFailure(Endpoint::Electrum, notif.payload, ::notification_error_kind(notif));
        finishEndpointConnection(Endpoint::Electrum, false, notif.payload,
                                 ::notification_error_kind(notif));
        m_reconnect->lost();
        emit endpointStateChanged(Endpoint::Electrum);
        break;
    case NotificationFlag::ElectrumStopped:
        finishEndpointConnection(Endpoint::Electrum, false, rust::String("Electrum stopped"),
                                 BackendErrorKind::Electrum);
        m_electrum_connected_count--;
        if (m_electrum_connected_count <= 0) {
            m_electrum_connected_count = 0;
            m_electrum_running = false;
            m_reconnect->lost();
            emit electrumDisconnected();
            emit endpointStateChanged(Endpoint::Electrum);
        }
        break;
    default:
        break;
    }
}

auto AccountController::loadPanels() -> void {
    m_panels_requested = true;
    if (m_init && !hasAccountMetadata()) {
        return;
    }
    if (!assembleServices()) {
        return;
    }
    auto services = account::servicesOf(*m_assembly);
    if (!m_panels.contains("settings")) {
        auto *settingsScreen = new account::view::Settings(services, nullptr);
        this->insertPanel(new qontrol::Panel(settingsScreen, "settings"));
    }
    if (m_panels.contains("history") ||
        (m_init && (!m_account.has_value() || !m_account.value()->is_ok()))) {
        return;
    }
    // The views open on the account's funds, not on an empty read.
    if (!m_services->fundsKnown()) {
        connect(m_services, &account::ServiceContext::fundsAnswered, this,
                &AccountController::onFundsAnswered, qontrol::UNIQUE);
        return;
    }
    insertPanel(new qontrol::Panel(new account::view::History(services), "history"));
    insertPanel(new qontrol::Panel(new account::view::Coins(services), "coins"));
    insertPanel(new qontrol::Panel(new account::view::Receive(services, module()), "receive"));
    insertPanel(new qontrol::Panel(new account::view::Send(services, new SpSigning(this, services)),
                                   "send"));
}

auto AccountController::historyClicked() -> void {
    this->loadPanel("history");
}

auto AccountController::coinsClicked() -> void {
    this->loadPanel("coins");
}

auto AccountController::sendClicked() -> void {
    this->loadPanel("send");
}

auto AccountController::receiveClicked() -> void {
    this->loadPanel("receive");
}

auto AccountController::settingsClicked() -> void {
    this->loadPanel("settings");
}

auto AccountController::stop() -> void {
    stopEndpointOperations();
    if (m_endpoint_busy || accountLoading()) {
        m_endpoint_stop_requested = true;
        return;
    }
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    ++m_notif_generation;
    m_reconnect->stopped();

    if (!m_account.has_value() && m_workers.isEmpty()) {
        // Nothing to tear down (mock controller or never initialized).
        onShutdownDrained({});
        return;
    }

    // Hand the account and the threads to a background thread so the GUI thread
    // never blocks. The workers use the account and the views: they end first.
    // Dropping the account disconnects the notification channel, which unblocks
    // the notification thread's recv(). Each thread is deleted only after the
    // wait, so none can be gone before the teardown waits on it, and stopped()
    // lets the owner delete the widget once everything is gone.
    auto account = std::move(m_account);
    m_account.reset();
    QThread *notif = m_notif_thread;
    m_notif_thread = nullptr;
    auto workers = std::move(m_workers);
    m_workers.clear();

    drainAccount(std::move(account), notif, workers);
}

void AccountController::startWorker(QThread *worker) {
    if (m_stopping || endpointsStopping() || accountDeletionPending()) {
        worker->deleteLater();
        return;
    }
    m_workers.append(worker);
    connect(worker, &QThread::finished, this, &AccountController::onWorkerFinished,
            qontrol::UNIQUE);
    worker->start();
}

// Once stop() took the workers, its teardown deletes them.
void AccountController::onWorkerFinished() {
    for (auto it = m_workers.begin(); it != m_workers.end();) {
        if ((*it)->isFinished()) {
            (*it)->deleteLater();
            it = m_workers.erase(it);
        } else {
            ++it;
        }
    }
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::getSpAddress() -> rust::String {
    if (m_account.has_value()) {
        return m_account.value()->sp_address();
    }
    return rust::String("");
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::network() -> Network {
    return accountConfig().get_network();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::hasSegwit() -> bool {
    return hasAccountMetadata() && !accountConfig().get_segwit_descriptor().empty();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::hasTaproot() -> bool {
    return hasAccountMetadata() && !accountConfig().get_taproot_descriptor().empty();
}

auto AccountController::getDescriptor() -> rust::String {
    return accountDescriptor();
}

auto AccountController::endpoints() -> QList<Endpoint> {
    return {Endpoint::Blindbit, Endpoint::Electrum};
}

auto AccountController::verifiesCertificate() const -> bool {
    return savedEndpointVerify();
}

auto AccountController::endpointUrl(Endpoint endpoint) -> QString {
    return savedEndpointUrl(endpoint);
}

auto AccountController::isEndpointRunning(Endpoint endpoint) const -> bool {
    return !endpointsStopping() &&
           (endpoint == Endpoint::Blindbit ? m_scanner_running : m_electrum_running);
}

void AccountController::onElectrumReconnect() {
    if (endpointsStopping()) {
        return;
    }
    if (endpointOperationsBusy()) {
        m_reconnect_pending = true;
    } else if (m_account.has_value()) {
        m_reconnect_pending = false;
        probeElectrum(true);
    }
}

void AccountController::connectElectrum(bool retry) {
    changeEndpoint(0, Endpoint::Electrum, true, retry, !retry);
}

// The account never left: only the reconnect follows a failed retry. At startup the
// failure is reported and the startup operation goes on, starting the scanner alone,
// unless a deletion started meanwhile.
void AccountController::electrumProbeFailed(bool retry, const EndpointResult &result) {
    if (!retry && !accountDeletionPending()) {
        m_reconnect->stopped();
        m_reconnect_pending = false;
        reportEndpointFailure(Endpoint::Electrum, result.error, result.kind);
        startEndpointChange(0, Endpoint::Blindbit, true, false, true);
        return;
    }
    finishEndpointOperation(result);
    if (retry) {
        m_reconnect->lost();
    }
    if (m_reconnect_pending && !endpointOperationsBusy()) {
        onElectrumReconnect();
    }
}

void AccountController::onSettingsIdle() {
    if (m_reconnect_pending && !endpointOperationsBusy()) {
        onElectrumReconnect();
    }
}

void AccountController::onElectrumGaveUp(int losses) {
    auto server = endpointUrl(Endpoint::Electrum);
    if (server.isEmpty()) {
        server = TR("electrum-the-server");
    }
    AppController::execModal(new catalog::MessageModal(
        TR("electrum-gave-up-title"), TR("electrum-gave-up-message").arg(server).arg(losses)));
}

auto AccountController::module() -> IModule * {
    return SpModule::get();
}

auto AccountController::services() const -> account::ServiceContext * {
    return m_services;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::coins() -> qontrol::Screen * {
    auto scr = screen("coins");
    if (scr.has_value()) {
        return scr.value();
    }
    return nullptr;
}

auto AccountController::getAccount() -> std::optional<rust::Box<Account>> & {
    return m_account;
}

void AccountController::requestTxDetails(uint64_t req_id, const QString &txid) {
    requestAccountTxDetails(req_id, txid, m_account.has_value() ? &*m_account.value() : nullptr,
                            isEndpointRunning(Endpoint::Electrum) && !m_endpoint_busy);
}

void AccountController::requestEndpointTip(uint64_t req_id, Endpoint endpoint) {
    if (!m_account.has_value() || endpointOperationsBusy() || !isEndpointRunning(endpoint)) {
        ElectrumTipResult result;
        result.is_ok = false;
        result.error = rust::String("Endpoint is disconnected");
        emit endpointTipReady(req_id, endpoint, result);
        return;
    }
    requestAccountEndpointTip(req_id, endpoint);
}

void AccountController::setEndpointConnected(uint64_t req_id, Endpoint endpoint, bool connected) {
    changeEndpoint(req_id, endpoint, connected);
}

void AccountController::changeEndpoint(uint64_t req_id, Endpoint endpoint, bool connected,
                                       bool retry, bool startup) {
    if (endpointOperationsBusy() || m_stopping || !m_account.has_value() ||
        !endpoints().contains(endpoint)) {
        EndpointResult result{};
        result.kind = BackendErrorKind::Other;
        result.is_ok = false;
        result.error = rust::String("Account or endpoint is not available");
        if (endpointsStopping()) {
            result.error = rust::String("Account is stopping");
        } else if (endpointOperationsBusy()) {
            result.error = rust::String("Endpoint operation is in progress");
        }
        emit endpointRequestFinished(req_id, endpoint, result);
        return;
    }
    startEndpointChange(req_id, endpoint, connected, retry, startup);
}

void AccountController::startEndpointChange(uint64_t req_id, Endpoint endpoint, bool connected,
                                            bool retry, bool startup) {
    m_endpoint_busy = true;
    m_endpoint_retry = retry;
    m_endpoint_startup = startup;
    m_endpoint_server = std::make_shared<BackendServer>(BackendServer::Electrum);
    m_endpoint_notifications.clear();
    m_endpoint = endpoint;
    if (!connected) {
        m_reconnect->stopped();
        m_reconnect_pending = false;
    }
    m_endpoint_account = std::make_shared<std::optional<rust::Box<Account>>>(std::move(m_account));
    m_account.reset();
    m_endpoint_result = std::make_shared<EndpointResult>();
    m_endpoint_replaced = std::make_shared<bool>(false);
    auto workers = std::move(m_workers);
    m_workers.clear();
    beginEndpointOperation(req_id, endpoint,
                           connected ? EndpointOperation::Connecting
                                     : EndpointOperation::Disconnecting);
    auto *thread = endpointChangeWorker(
        m_endpoint_account, m_endpoint_result, m_endpoint_replaced, workers, m_notif_thread,
        [endpoint, connected, startup](rust::Box<Account> account) {
            if (startup && endpoint == Endpoint::Electrum) {
                return ::sp_startup_endpoint_change(std::move(account));
            }
            return ::sp_endpoint_change(std::move(account),
                                        endpoint == Endpoint::Electrum ? BackendServer::Electrum
                                                                       : BackendServer::Blindbit,
                                        connected);
        },
        ::take_sp_endpoint_account, m_endpoint_server);
    connect(thread, &QThread::finished, this, &AccountController::onEndpointFinished,
            qontrol::UNIQUE);
    startWorker(thread);
}

void AccountController::onEndpointFinished() {
    m_account = std::move(*m_endpoint_account);
    m_endpoint_account = nullptr;
    m_endpoint_busy = false;
    if (m_account.has_value() && m_account.value()->is_ok()) {
        m_electrum_expected_count = static_cast<int>(m_account.value()->sub_account_count());
        setAccountDescriptor(m_account.value()->descriptor());
        if (m_panels_requested) {
            loadPanels();
        }
        setSavedEndpoint(Endpoint::Blindbit, m_account.value()->blindbit_url(), true);
        setSavedEndpoint(Endpoint::Electrum, m_account.value()->electrum_url(),
                         m_account.value()->verify_certificate());
    }
    if (*m_endpoint_replaced) {
        if (!m_endpoint_retry) {
            m_reconnect->stopped();
            m_reconnect_pending = false;
        }
        m_notif_thread = nullptr;
        m_scanner_running = false;
        m_electrum_running = false;
        m_electrum_connected_count = 0;
        if (m_account.has_value() && m_account.value()->is_ok()) {
            listenNotifications();
        }
        emit scannerStateChanged(false);
        emit electrumDisconnected();
        emit endpointStateChanged(Endpoint::Blindbit);
        emit endpointStateChanged(Endpoint::Electrum);
    }
    if (m_endpoint_startup && !endpointsStopping()) {
        m_endpoint =
            *m_endpoint_server == BackendServer::Blindbit ? Endpoint::Blindbit : Endpoint::Electrum;
        beginEndpointOperation(0, m_endpoint, EndpointOperation::Connecting);
    }
    auto result = *m_endpoint_result;
    // BlindBit completes on scanner startup, not the other endpoint's Electrum handshake.
    if (m_endpoint == Endpoint::Blindbit) {
        result.connection_pending =
            result.is_ok && endpointOperation(m_endpoint) == EndpointOperation::Connecting;
    }
    auto notifications = std::move(m_endpoint_notifications);
    m_endpoint_notifications.clear();
    bool replaced = *m_endpoint_replaced;
    bool retry = m_endpoint_retry;
    finishEndpointOperation(result);
    if (retry && !result.is_ok && !endpointsStopping()) {
        m_reconnect->lost();
    }
    if (!replaced && !endpointsStopping()) {
        for (const auto &notification : notifications) {
            handleNotification(notification);
        }
    }
    if (m_endpoint_stop_requested) {
        stop();
    } else if (m_reconnect_pending && !endpointOperationsBusy()) {
        onElectrumReconnect();
    }
}
