#include "modules/musig/MusigAccountController.h"

#include "account/views/Coins.h"
#include "account/views/History.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/views/Receive.h"
#include "modules/musig/views/Send.h"
#include "modules/musig/views/Settings.h"
#include "views/utils.h"

#include <QDebug>
#include <QThread>
#include <common.h>
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
      m_widget(widget) {
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

    // Before any view connects, so the controller state is clear when views react.
    connect(this, &MusigAccountController::broadcastDone, this,
            &MusigAccountController::onBroadcastResult, qontrol::UNIQUE);

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
    m_panels.insert("history", new qontrol::Panel(new account::view::History(this), "history"));
    m_panels.insert("coins", new qontrol::Panel(new account::view::Coins(this), "coins"));
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
    if (!m_account.has_value() || m_psbt_v2_hex.isEmpty()) {
        return;
    }
    // Pure computation, as sp prepares on the GUI thread.
    std::string psbt = m_psbt_v2_hex.toStdString();
    auto result = m_account.value()->finalize_psbt(rust::Str(psbt));
    if (!result.is_ok) {
        emit finalizeFailed(toQString(result.error));
        return;
    }
    m_tx_hex = toQString(result.value);
    emit transactionReady(m_tx_hex);
}

// The prepared PSBT is already restricted to the leaf and holds no nonce and no
// partial sig.
void MusigAccountController::restartRounds() {
    m_psbt_v2_hex = m_prepared_psbt_v2_hex;
    m_tx_hex.clear();
    emit roundChanged(roundStatus());
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

auto MusigAccountController::setElectrumUrl(const QString &url) -> MusigElectrumUrlResult {
    if (!m_account.has_value()) {
        return MusigElectrumUrlResult{.saved = false, .started = false};
    }
    auto result = m_account.value()->set_electrum_url(rust::String(url.toStdString()));
    if (!result.saved) {
        qWarning() << "MusigAccountController: failed to save electrum url" << url;
        return result;
    }
    emit electrumUrlChanged();
    if (!result.started) {
        // No listener runs on this url, so none reports its state.
        emit electrumDisconnected();
    }
    return result;
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
    // A running broadcast uses the account: it ends before the account drops.
    QThread *broadcast = m_broadcast_thread;
    m_broadcast_thread = nullptr;

    auto *teardown = QThread::create([acc = std::move(account), notif, broadcast]() mutable {
        if (broadcast != nullptr) {
            broadcast->wait();
            broadcast->deleteLater();
        }
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
    if (m_account.has_value()) {
        return m_account.value()->simulate_transaction(std::move(tx));
    }
    TransactionSimulation simulation;
    simulation.is_valid = false;
    simulation.error = rust::String(NOT_LOADED);
    return simulation;
}

void MusigAccountController::prepareTransaction(TransactionTemplate tx, MusigSpendLeaf spend) {
    if (!m_account.has_value()) {
        emit prepareFailed(TR("send-no-account-loaded"));
        return;
    }
    auto psbt = m_account.value()->prepare_transaction(std::move(tx), std::move(spend));
    if (!psbt->is_ok()) {
        emit prepareFailed(mapBackendErrorSummary(toQString(psbt->get_psbt_error())));
        return;
    }
    m_prepared_psbt_v2_hex = toQString(psbt->bytes_v2_hex());
    m_psbt_v2_hex = m_prepared_psbt_v2_hex;
    m_tx_hex.clear();
    emit prepared(m_psbt_v2_hex, toQString(psbt->get_txid_preview()));
    emit roundChanged(roundStatus());
}

void MusigAccountController::signRound() {
    if (m_psbt_v2_hex.isEmpty() || m_sign_req_id != 0 || !m_account.has_value()) {
        return;
    }
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        emit signingFailed(TR("musig-send-no-keys"), plugin::SignError::Other);
        return;
    }
    connect(keys, &IKeys::signed_, this, &MusigAccountController::onKeysSigned, qontrol::UNIQUE);
    connect(keys, &IKeys::error, this, &MusigAccountController::onKeysError, qontrol::UNIQUE);
    connect(keys, &IKeys::cancelled, this, &MusigAccountController::onKeysCancelled,
            qontrol::UNIQUE);
    // The answer may arrive during sign(): the id is stored first.
    m_sign_req_id = keys->newRequest();
    keys->sign(m_sign_req_id, network(), m_psbt_v2_hex, QStringList{toQString(getDescriptor())},
               toQString(m_account.value()->name()));
}

void MusigAccountController::cancelSigning() {
    m_prepared_psbt_v2_hex.clear();
    m_psbt_v2_hex.clear();
    m_tx_hex.clear();
    m_sign_req_id = 0;
}

void MusigAccountController::broadcast() {
    if (m_tx_hex.isEmpty() || !m_account.has_value() || m_broadcast_thread != nullptr) {
        return;
    }
    // stop() waits on this thread before it drops the account and before stopped()
    // lets the owner delete this controller, so both outlive the worker.
    const MusigAccount *account = &*m_account.value();
    m_broadcast_thread = QThread::create([this, account, tx = m_tx_hex.toStdString()]() {
        auto result = account->broadcast_transaction(rust::String(tx));
        emit broadcastDone(result);
    });
    connect(m_broadcast_thread, &QThread::finished, this,
            &MusigAccountController::onBroadcastThreadFinished, qontrol::UNIQUE);
    m_broadcast_thread->start();
}

void MusigAccountController::onKeysSigned(ReqId req_id, const QString &psbt_v2_hex) {
    if (req_id != m_sign_req_id) {
        return;
    }
    m_sign_req_id = 0;
    m_psbt_v2_hex = psbt_v2_hex;
    auto status = roundStatus();
    if (!status.is_ok) {
        emit signingFailed(toQString(status.error), plugin::SignError::Other);
        return;
    }
    emit roundChanged(status);
    // A single-key leaf signs in one call.
    if (status.round == MusigRound::Ready || status.round == MusigRound::NotMusig) {
        finalizeIfReady();
    }
}

void MusigAccountController::onKeysError(std::optional<ReqId> req_id, const QString &message,
                                         plugin::SignError kind) {
    if (!req_id.has_value() || req_id.value() != m_sign_req_id) {
        return;
    }
    m_sign_req_id = 0;
    if (kind == plugin::SignError::MusigSession) {
        restartRounds();
    }
    emit signingFailed(message, kind);
}

// The PSBT keeps the round it was at.
void MusigAccountController::onKeysCancelled(ReqId req_id) {
    if (req_id != m_sign_req_id) {
        return;
    }
    m_sign_req_id = 0;
    emit signingCancelled();
}

void MusigAccountController::onBroadcastResult(const TxResult &result) {
    if (result.is_ok) {
        cancelSigning();
    }
    pollCoins();
}

// Once stop() took the thread, its teardown deletes it.
void MusigAccountController::onBroadcastThreadFinished() {
    if (m_broadcast_thread != nullptr) {
        m_broadcast_thread->deleteLater();
        m_broadcast_thread = nullptr;
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
