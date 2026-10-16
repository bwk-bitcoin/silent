#include "modules/musig/MusigAccountController.h"

#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/views/Coins.h"
#include "modules/musig/views/History.h"
#include "modules/musig/views/Receive.h"
#include "modules/musig/views/Send.h"
#include "modules/musig/views/Settings.h"

#include <QDebug>
#include <QThread>
#include <common.h>
#include <string>
#include <utility>

MusigAccountController::MusigAccountController(const QString &account, MusigAccountWidget *widget)
    : m_widget(widget) {
    init(account);
}

void MusigAccountController::init(const QString &account) {
    if (m_init) {
        return;
    }

    auto acc = ::new_musig_account(rust::String(account.toStdString()));
    if (!acc->is_ok()) {
        qCritical() << "MusigAccountController: failed to load account" << account
                    << QString::fromStdString(std::string(acc->get_error().c_str()));
        return;
    }
    m_account = std::make_optional(std::move(acc));

    // Notifications come from a blocking recv() on a dedicated thread.
    connect(this, &MusigAccountController::notificationReceived, this,
            &MusigAccountController::handleNotification, qontrol::UNIQUE);
    auto receiver = m_account.value()->take_receiver();
    m_notif_thread = QThread::create([this, recv = std::move(receiver)]() {
        while (true) {
            auto poll = recv->recv();
            if (!poll->is_some()) {
                break;
            }
            emit notificationReceived(poll->get_notification());
        }
    });
    m_notif_thread->start();

    m_init = true;
}

void MusigAccountController::loadPanels() {
    m_panels.insert("history", new qontrol::Panel(new musig::view::History(this), "history"));
    m_panels.insert("coins", new qontrol::Panel(new musig::view::Coins(this), "coins"));
    m_panels.insert("receive", new qontrol::Panel(new musig::view::Receive(this), "receive"));
    m_panels.insert("send", new qontrol::Panel(new musig::view::Send(this), "send"));
    m_panels.insert("settings", new qontrol::Panel(new musig::view::Settings(this), "settings"));
}

auto MusigAccountController::screen(const QString &screen) -> std::optional<qontrol::Screen *> {
    auto *panel = m_panels.value(screen);
    if (panel != nullptr) {
        return std::optional(panel->screen());
    }
    return std::nullopt;
}

auto MusigAccountController::getAccount() -> std::optional<rust::Box<MusigAccount>> & {
    return m_account;
}

// Unspent coins only.
auto MusigAccountController::getCoins() -> rust::Vec<RustCoin> {
    rust::Vec<RustCoin> unspent;
    if (!m_account.has_value()) {
        return unspent;
    }
    for (auto &coin : m_account.value()->coins()) {
        if (!coin.spent) {
            unspent.push_back(std::move(coin));
        }
    }
    return unspent;
}

auto MusigAccountController::getPaymentHistory() -> rust::Vec<RustTx> {
    if (m_account.has_value()) {
        return m_account.value()->payment_history();
    }
    return {};
}

auto MusigAccountController::newAddress() -> rust::String {
    if (m_account.has_value()) {
        return m_account.value()->new_addr();
    }
    return {};
}

auto MusigAccountController::network() -> Network {
    return m_account.has_value() ? m_account.value()->network() : Network::Regtest;
}

auto MusigAccountController::getDescriptor() -> rust::String {
    if (m_account.has_value()) {
        return m_account.value()->descriptor();
    }
    return {};
}

auto MusigAccountController::cosignerFingerprints() -> QStringList {
    QStringList out;
    if (!m_account.has_value()) {
        return out;
    }
    for (const auto &fingerprint : m_account.value()->cosigner_fingerprints()) {
        out.append(QString::fromStdString(std::string(fingerprint)));
    }
    return out;
}

auto MusigAccountController::addressVerifyInfo(const QString &address) -> AddressVerifyInfo {
    if (m_account.has_value()) {
        std::string addr = address.toStdString();
        return m_account.value()->address_verify_info(rust::Str(addr));
    }
    return AddressVerifyInfo{.descriptor = rust::String(""), .path = rust::String("")};
}

auto MusigAccountController::electrumUrl() -> QString {
    if (m_account.has_value()) {
        return QString::fromStdString(std::string(m_account.value()->electrum_url().c_str()));
    }
    return {};
}

auto MusigAccountController::roundStatus() const -> MusigRoundStatus {
    return {};
}

auto MusigAccountController::psbtInFlight() const -> QString {
    return {};
}

auto MusigAccountController::spendLeaves(const QStringList &outpoints) -> MusigSpendLeaves {
    Q_UNUSED(outpoints);
    return {};
}

void MusigAccountController::finalizeIfReady() {
}

void MusigAccountController::restartRounds() {
}

// ===== Slots =====

void MusigAccountController::loadPanel(const QString &name) {
    auto *panel = m_panels.value(name);
    if (m_widget != nullptr && panel != nullptr) {
        m_widget->loadPanel(panel);
    }
}

void MusigAccountController::pollCoins() {
    if (m_account.has_value()) {
        emit updateCoins(m_account.value()->spendable_coins());
        emit updateBalance(m_account.value()->balance());
    }
}

void MusigAccountController::handleNotification(Notification notif) {
    switch (notif.flag) {
    case NotificationFlag::NewOutput:
    case NotificationFlag::OutputSpent:
    case NotificationFlag::CoinUpdate:
    case NotificationFlag::PaymentUpdated:
    case NotificationFlag::AddressTipChanged:
        pollCoins();
        break;
    case NotificationFlag::ElectrumConnected:
        emit electrumConnected(QString::fromStdString(std::string(notif.payload.c_str())));
        break;
    case NotificationFlag::ElectrumError:
        qWarning() << "musig: electrum:"
                   << QString::fromStdString(std::string(notif.payload.c_str()));
        emit electrumDisconnected();
        break;
    case NotificationFlag::ElectrumStopped:
        emit electrumDisconnected();
        break;
    default:
        break;
    }
}

void MusigAccountController::updateCoinLabel(const QString &outpoint, const QString &label) {
    if (!m_account.has_value()) {
        return;
    }
    if (!m_account.value()->update_coin_label(rust::String(outpoint.toStdString()),
                                              rust::String(label.toStdString()))) {
        qWarning() << "MusigAccountController: failed to update coin label for" << outpoint;
    }
    pollCoins();
}

void MusigAccountController::setElectrumUrl(const QString &url) {
    if (!m_account.has_value()) {
        return;
    }
    if (!m_account.value()->set_electrum_url(rust::String(url.toStdString()))) {
        qWarning() << "MusigAccountController: failed to set electrum url" << url;
        return;
    }
    emit electrumUrlChanged();
}

void MusigAccountController::startElectrum() {
    if (m_account.has_value()) {
        m_account.value()->start_electrum();
    }
}

void MusigAccountController::stopElectrum() {
    if (m_account.has_value()) {
        m_account.value()->stop_electrum();
    }
}

void MusigAccountController::stop() {
    if (m_stopping) {
        return;
    }
    m_stopping = true;

    if (!m_account.has_value()) {
        emit stopped();
        return;
    }

    // The account and the notification thread go to a background thread so the GUI
    // never blocks: dropping the account closes the channel, which ends the listener's
    // recv(); stopped() fires once the listener has exited. The listener is deleted
    // only after the wait, so it cannot be gone before the teardown waits on it.
    auto account = std::move(m_account.value());
    m_account.reset();
    QThread *notif = m_notif_thread;
    m_notif_thread = nullptr;

    auto *teardown = QThread::create([acc = std::move(account), notif]() mutable {
        acc->stop_electrum();
        {
            auto dropped = std::move(acc);
        }
        if (notif != nullptr) {
            notif->wait();
            notif->deleteLater();
        }
    });
    connect(teardown, &QThread::finished, this, &MusigAccountController::stopped, qontrol::UNIQUE);
    connect(teardown, &QThread::finished, teardown, &QThread::deleteLater);
    teardown->start();
}

auto MusigAccountController::simulateTx(TransactionTemplate tx) -> TransactionSimulation {
    Q_UNUSED(tx);
    return {};
}

void MusigAccountController::prepareTransaction(TransactionTemplate tx, MusigSpendLeaf spend) {
    Q_UNUSED(tx);
    Q_UNUSED(spend);
}

void MusigAccountController::signRound() {
}

void MusigAccountController::cancelSigning() {
}

void MusigAccountController::broadcast() {
}

void MusigAccountController::onKeysSigned(ReqId req_id, const QString &psbt_v2_hex) {
    Q_UNUSED(req_id);
    Q_UNUSED(psbt_v2_hex);
}

void MusigAccountController::onKeysError(std::optional<ReqId> req_id, const QString &message,
                                         plugin::SignError kind) {
    Q_UNUSED(req_id);
    Q_UNUSED(message);
    Q_UNUSED(kind);
}

void MusigAccountController::onKeysCancelled(ReqId req_id) {
    Q_UNUSED(req_id);
}

void MusigAccountController::onBroadcastResult(TxResult result) {
    Q_UNUSED(result);
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
