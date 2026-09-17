#include "AccountController.h"
#include "AccountWidget.h"
#include "AppController.h"
#include "views/Coins.h"
#include "views/History.h"
#include "views/Receive.h"
#include "views/Send.h"
#include "views/Settings.h"
#include <Qontrol>
#include <common.h>
#include <qlogging.h>
#include <string>
#include <utility>

AccountController::AccountController(const QString &account, AccountWidget *widget) {
    m_widget = widget;
    init(account);
}

AccountController::AccountController(AccountWidget *widget) {
    m_widget = widget;
}

auto AccountController::init(const QString &account) -> void {
    if (m_init)
        return;

    // Initialize the account
    auto acc = new_account(rust::String(account.toStdString()));
    if (!acc->is_ok()) {
        qCritical() << "Failed to create account:" << account
                    << QString::fromStdString(std::string(acc->get_error().c_str()));
        return;
    }
    m_account = std::make_optional(std::move(acc));
    m_electrum_expected_count = static_cast<int>(m_account.value()->sub_account_count());

    // Take the notification receiver and spawn a blocking listener thread
    connect(this, &AccountController::notificationReceived, this,
            &AccountController::handleNotification, qontrol::UNIQUE);
    auto receiver = m_account.value()->take_receiver();
    m_notif_thread = QThread::create([this, recv = std::move(receiver)]() {
        while (true) {
            auto poll = recv->recv();
            if (!poll->is_some()) {
                break;
            }
            auto notif = poll->get_notification();
            emit notificationReceived(notif);
        }
    });
    connect(m_notif_thread, &QThread::finished, m_notif_thread, &QThread::deleteLater);
    m_notif_thread->start();

    m_init = true;
}

auto AccountController::loadPanel(const QString &name) -> void {
    auto *panel = m_panels.value(name);
    if (m_widget != nullptr && panel != nullptr) {
        m_widget->loadPanel(panel);
    }
}

auto AccountController::insertPanel(qontrol::Panel *panel) -> void {
    m_panels.insert(panel->name(), panel);
}

auto AccountController::screen(const QString &screen) -> std::optional<qontrol::Screen *> {
    auto *panel = m_panels.value(screen);
    if (panel != nullptr) {
        return std::optional(panel->screen());
    }
    return std::nullopt;
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

auto AccountController::handleNotification(Notification notif) -> void {
    auto flag = notif.flag;

    switch (flag) {
    case NotificationFlag::ScanProgress: {
        auto payload = QString::fromStdString(std::string(notif.payload.c_str()));
        auto parts = payload.split(",");
        if (parts.size() == 2) {
            bool ok1 = false;
            bool ok2 = false;
            uint32_t height = parts[0].toUInt(&ok1);
            uint32_t tip = parts[1].toUInt(&ok2);
            if (ok1 && ok2) {
                m_estimator->update(height, tip);
                emit scanProgress(height, tip);
            } else {
                emit scanProgress(0, 0);
            }
        } else {
            emit scanProgress(0, 0);
        }
        break;
    }
    case NotificationFlag::NewOutput:
    case NotificationFlag::OutputSpent:
        pollCoins();
        break;
    case NotificationFlag::FailStartScanning:
        emit scanError(notif.payload);
        break;
    case NotificationFlag::FailScan:
        emit scanError(notif.payload);
        break;
    case NotificationFlag::StartingScan:
        m_scanner_running = true;
        m_estimator->reset();
        emit scannerStateChanged(true);
        break;
    case NotificationFlag::ScanStarted:
        m_scanner_running = true;
        emit scannerStateChanged(true);
        break;
    case NotificationFlag::StoppingScan:
        break;
    case NotificationFlag::ScanCompleted:
        break;
    case NotificationFlag::ScanStopped:
        m_scanner_running = false;
        m_estimator->reset();
        emit scannerStateChanged(false);
        break;
    case NotificationFlag::WaitingForBlocks: {
        auto payload = QString::fromStdString(std::string(notif.payload.c_str()));
        bool ok = false;
        uint32_t tipHeight = payload.toUInt(&ok);
        if (ok) {
            emit waitingForBlocks(tipHeight);
        }
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
            emit electrumConnected(payload);
        }
        break;
    }
    case NotificationFlag::ElectrumError:
        emit scanError(notif.payload);
        break;
    case NotificationFlag::ElectrumStopped:
        m_electrum_connected_count--;
        if (m_electrum_connected_count <= 0) {
            m_electrum_connected_count = 0;
            emit electrumDisconnected();
        }
        break;
    default:
        break;
    }
}

auto AccountController::loadPanels() -> void {
    // Create History screen panel
    auto *historyScreen = new view::History(this);
    auto *historyPanel = new qontrol::Panel(historyScreen, "history");
    this->insertPanel(historyPanel);

    // Create Coins screen panel
    auto *coinsScreen = new view::Coins(this);
    auto *coinsPanel = new qontrol::Panel(coinsScreen, "coins");
    this->insertPanel(coinsPanel);

    // Create Receive screen panel
    auto *receiveScreen = new view::Receive(this);
    auto *receivePanel = new qontrol::Panel(receiveScreen, "receive");
    this->insertPanel(receivePanel);

    // Create Send screen panel
    auto *sendScreen = new view::Send(this);
    auto *sendPanel = new qontrol::Panel(sendScreen, "send");
    this->insertPanel(sendPanel);

    // Create Settings screen panel
    auto *settingsScreen = new view::Settings(this);
    auto *settingsPanel = new qontrol::Panel(settingsScreen, "settings");
    this->insertPanel(settingsPanel);
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

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::simulateTx(TransactionTemplate tx) -> TransactionSimulation {
    if (m_account.has_value()) {
        return m_account.value()->simulate_transaction(std::move(tx));
    }
    qDebug() << "AccountController::simulateTx() m_account is None!";
    // Return empty/invalid simulation
    TransactionSimulation sim;
    sim.is_valid = false;
    sim.error = rust::String("Account not initialized");
    return sim;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::startScanner() -> void {
    if (m_account.has_value()) {
        m_account.value()->start_scanner();
    }
}

auto AccountController::stop() -> void {
    if (m_stopping) {
        return;
    }
    m_stopping = true;

    if (!m_account.has_value()) {
        // Nothing to tear down (mock controller or never initialized).
        emit stopped();
        return;
    }

    // Hand the account and notification thread to a background thread so the GUI
    // thread never blocks. Dropping the account disconnects the notification
    // channel, which unblocks the notification thread's recv(); the teardown
    // thread then waits for it to exit and emits stopped() so the owner can
    // delete the widget once everything is gone.
    auto account = std::move(m_account.value());
    m_account.reset();
    QThread *notif = m_notif_thread;
    m_notif_thread = nullptr;

    auto *teardown = QThread::create([acc = std::move(account), notif]() mutable {
        acc->stop_scanner();
        acc->stop_electrum();
        { auto dropped = std::move(acc); } // drop the account, disconnecting the channel
        if (notif != nullptr) {
            notif->wait();
        }
    });
    connect(teardown, &QThread::finished, this, &AccountController::stopped, qontrol::UNIQUE);
    connect(teardown, &QThread::finished, teardown, &QThread::deleteLater);
    teardown->start();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::getCoins() -> rust::Vec<RustCoin> {
    if (m_account.has_value()) {
        auto all = m_account.value()->coins();
        rust::Vec<RustCoin> spendable;
        for (auto &coin : all) {
            if (!coin.spent) {
                spendable.push_back(std::move(coin));
            }
        }
        return spendable;
    }
    return rust::Vec<RustCoin>();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::getPaymentHistory() -> rust::Vec<RustTx> {
    if (m_account.has_value()) {
        return m_account.value()->payment_history();
    }
    return rust::Vec<RustTx>();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::getSpAddress() -> rust::String {
    if (m_account.has_value()) {
        return m_account.value()->sp_address();
    }
    return rust::String("");
}

auto AccountController::newSegwitAddr() -> rust::String {
    if (m_account.has_value()) {
        return m_account.value()->new_segwit_addr();
    }
    return rust::String("");
}

auto AccountController::newTaprootAddr() -> rust::String {
    if (m_account.has_value()) {
        return m_account.value()->new_taproot_addr();
    }
    return rust::String("");
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::hasSubAccounts() -> bool {
    return m_account.has_value() && m_account.value()->has_sub_accounts();
}

auto AccountController::getDescriptor() -> rust::String {
    if (m_account.has_value()) {
        return m_account.value()->descriptor();
    }
    return rust::String("");
}

auto AccountController::addressVerifyInfo(const QString &address) -> AddressVerifyInfo {
    if (m_account.has_value()) {
        return m_account.value()->address_verify_info(rust::String(address.toStdString()));
    }
    return AddressVerifyInfo{.descriptor = rust::String(""), .path = rust::String("")};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AccountController::coins() -> qontrol::Screen * {
    auto scr = screen("coins");
    if (scr.has_value()) {
        return scr.value();
    }
    return nullptr;
}

auto AccountController::updateCoinLabel(const QString &outpoint, const QString &label) -> void {
    if (m_account.has_value()) {
        if (!m_account.value()->update_coin_label(rust::String(outpoint.toStdString()),
                                                  rust::String(label.toStdString()))) {
            qWarning() << "Failed to update coin label for:" << outpoint;
        }
        pollCoins();
    }
}

auto AccountController::getAccount() -> std::optional<rust::Box<Account>> & {
    return m_account;
}
