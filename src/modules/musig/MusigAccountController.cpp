#include "modules/musig/MusigAccountController.h"

#include "AppController.h"
#include "account/views/Coins.h"
#include "account/views/History.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "account/views/Settings.h"
#include "catalog/feedback/MessageModal.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigModule.h"
#include "modules/musig/SavedPsbts.h"
#include "modules/musig/SettingsExtras.h"
#include "modules/musig/SigningSection.h"
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
    connect(m_reconnect, &account::ElectrumReconnect::reconnect, this,
            &MusigAccountController::onElectrumReconnect, qontrol::UNIQUE);
    connect(m_reconnect, &account::ElectrumReconnect::gaveUp, this,
            &MusigAccountController::onElectrumGaveUp, qontrol::UNIQUE);

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
    m_panels.insert("receive", new qontrol::Panel(new account::view::Receive(this), "receive"));
    auto *send = new account::view::Send(this);
    m_panels.insert("send", new qontrol::Panel(send, "send"));
    connect(this, &MusigAccountController::savedPsbtWarningChanged, send,
            &account::view::Send::onSavedWarningChanged, qontrol::UNIQUE);
    m_panels.insert("psbt", new qontrol::Panel(new musig::SavedPsbts(this), "psbt"));
    m_panels.insert("settings", new qontrol::Panel(new account::view::Settings(this), "settings"));
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

auto MusigAccountController::addressKinds() -> QList<AddressKind> {
    return {AddressKind::Taproot};
}

auto MusigAccountController::newAddress([[maybe_unused]] AddressKind kind) -> QString {
    if (m_account.has_value()) {
        return toQString(m_account.value()->new_addr());
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

auto MusigAccountController::endpoints() -> QList<Endpoint> {
    return {Endpoint::Electrum};
}

auto MusigAccountController::endpointUrl([[maybe_unused]] Endpoint endpoint) -> QString {
    return electrumUrl();
}

auto MusigAccountController::verifiesCertificate() const -> bool {
    return !m_account.has_value() || m_account.value()->verify_certificate();
}

auto MusigAccountController::isEndpointRunning([[maybe_unused]] Endpoint endpoint) const -> bool {
    return m_electrum_running;
}

auto MusigAccountController::startEndpoint([[maybe_unused]] Endpoint endpoint) -> bool {
    return m_account.has_value() && m_account.value()->start_electrum();
}

void MusigAccountController::stopEndpoint([[maybe_unused]] Endpoint endpoint) {
    stopElectrum();
}

auto MusigAccountController::networkEditable() const -> bool {
    return false;
}

auto MusigAccountController::saveSettings(const QHash<Endpoint, QString> &urls,
                                          [[maybe_unused]] Network network) -> QString {
    auto url = urls.value(Endpoint::Electrum);
    auto result = setElectrumUrl(url);
    if (!result.saved) {
        return TR("musig-settings-save-failed");
    }
    if (!url.isEmpty() && !result.started) {
        return TR("musig-settings-invalid-url");
    }
    return {};
}

auto MusigAccountController::module() -> IModule * {
    return MusigModule::get();
}

auto MusigAccountController::settingsExtras() -> QWidget * {
    return new musig::SettingsExtras(this);
}

auto MusigAccountController::sendSigning() -> account::SendSigning * {
    return new musig::SigningSection(this);
}

void MusigAccountController::startWorker(QThread *worker) {
    m_workers.append(worker);
    connect(worker, &QThread::finished, this, &MusigAccountController::onWorkerFinished,
            qontrol::UNIQUE);
    worker->start();
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

auto MusigAccountController::spendSigners() const -> QStringList {
    return m_spend_signers;
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
    auto result = std::make_shared<MusigFinalizeResult>();
    auto *account = &*m_account.value();
    enqueueStorage([account, result,
                    psbt = m_psbt_v2_hex.toStdString()] { *result = account->finalize_psbt(psbt); },
                   [this, result, generation] {
                       if (generation != m_signing_generation) {
                           return;
                       }
                       m_finalizing = false;
                       if (!result->is_ok) {
                           emit finalizeFailed(toQString(result->error));
                           return;
                       }
                       m_psbt_v2_hex = toQString(result->psbt_v2_hex);
                       m_tx_hex = toQString(result->tx_hex);
                       setSaveWarning(toQString(result->save_error));
                       emit savedPsbtsChanged();
                       emit transactionReady(m_tx_hex);
                   });
}

// Serialize the durable archive, retirement and new attempt behind progress writes.
void MusigAccountController::restartRounds() {
    if (m_resetting || m_psbt_v2_hex.isEmpty() || m_finalizing || !m_sign_reqs.isEmpty() ||
        !m_tx_hex.isEmpty() || !m_account.has_value() || m_saved_record_id.isEmpty() ||
        m_stopping || m_stop_requested) {
        return;
    }
    m_resetting = true;
    m_restart_generation = ++m_signing_generation;
    m_restart_source = m_psbt_v2_hex;
    m_restart_result = std::make_shared<SavedPsbtReset>();
    auto result = m_restart_result;
    auto *account = &*m_account.value();
    enqueueStorage(
        [account, result, id = m_saved_record_id.toStdString()] {
            *result = account->reset_saved_psbt(id);
        },
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

void MusigAccountController::handleNotification(Notification notif) {
    switch (notif.flag) {
    case NotificationFlag::NewOutput:
    case NotificationFlag::OutputSpent:
    case NotificationFlag::CoinUpdate:
    case NotificationFlag::PaymentUpdated:
    case NotificationFlag::AddressTipChanged:
        pollCoins();
        refreshActiveWarning();
        break;
    case NotificationFlag::ElectrumConnected:
        m_electrum_running = true;
        m_reconnect->connected();
        emit electrumConnected(QString::fromStdString(std::string(notif.payload.c_str())));
        emit endpointStateChanged(Endpoint::Electrum);
        break;
    case NotificationFlag::ElectrumError:
        qWarning() << "musig: electrum:"
                   << QString::fromStdString(std::string(notif.payload.c_str()));
        m_electrum_running = false;
        m_reconnect->lost();
        emit electrumDisconnected();
        emit endpointStateChanged(Endpoint::Electrum);
        break;
    case NotificationFlag::ElectrumStopped:
        m_electrum_running = false;
        m_reconnect->lost();
        emit electrumDisconnected();
        emit endpointStateChanged(Endpoint::Electrum);
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
    // The old listener's stop is the url change, not a lost connection.
    m_reconnect->stopped();
    auto result = m_account.value()->set_electrum_url(rust::String(url.toStdString()));
    if (!result.saved) {
        qWarning() << "MusigAccountController: failed to save electrum url" << url;
        return result;
    }
    emit electrumUrlChanged();
    if (!result.started) {
        // No listener runs on this url, so none reports its state.
        m_electrum_running = false;
        emit electrumDisconnected();
        emit endpointStateChanged(Endpoint::Electrum);
    }
    return result;
}

void MusigAccountController::startElectrum() {
    if (m_account.has_value()) {
        m_account.value()->start_electrum();
    }
}

void MusigAccountController::stopElectrum() {
    // Stopped on purpose: not reconnected.
    m_reconnect->stopped();
    if (m_account.has_value()) {
        m_account.value()->stop_electrum();
    }
}

void MusigAccountController::onElectrumReconnect() {
    if (m_account.has_value() && !m_stopping) {
        qInfo() << "musig: electrum: reconnecting";
        m_account.value()->start_electrum();
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
    cancelSigning();
    m_reconnect->stopped();

    if (!m_account.has_value() && m_workers.isEmpty()) {
        emit stopped();
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

    auto *teardown = QThread::create([acc = std::move(account), notif, workers]() mutable {
        for (auto *worker : workers) {
            worker->wait();
            worker->deleteLater();
        }
        if (acc.has_value()) {
            acc.value()->stop_electrum();
            acc.reset(); // drop the account, disconnecting the channel
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
    QStringList signers;
    for (const auto &fingerprint : spend.fingerprints) {
        signers.append(toQString(fingerprint));
    }
    cancelSigning();
    emit sessionLoading();
    auto generation = m_signing_generation;
    auto result = std::make_shared<std::optional<rust::Box<PsbtResult>>>();
    auto *account = &*m_account.value();
    enqueueStorage(
        [account, result, tx = std::move(tx), spend = std::move(spend)] {
            result->emplace(account->prepare_transaction(tx, spend));
        },
        [this, result, signers, generation] {
            emit savedPsbtsChanged();
            if (generation != m_signing_generation) {
                return;
            }
            auto &psbt = result->value();
            if (!psbt->is_ok()) {
                emit prepareFailed(mapBackendErrorSummary(toQString(psbt->get_psbt_error())));
                return;
            }
            m_psbt_v2_hex = toQString(psbt->bytes_v2_hex());
            m_saved_record_id = toQString(psbt->saved_record_id());
            m_spend_signers = signers;
            setSaveWarning(toQString(psbt->save_error()));
            emit prepared(m_psbt_v2_hex, toQString(psbt->get_txid_preview()));
            emit roundChanged(roundStatus());
        });
}

auto MusigAccountController::beginSignRequest(const QString &fingerprint)
    -> std::pair<IKeys *, ReqId> {
    if (m_restart_pending) {
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
    keys->sign(req, network(), m_psbt_v2_hex, QStringList{toQString(getDescriptor())},
               toQString(m_account.value()->name()), fingerprint);
}

void MusigAccountController::signRoundOnDevice(const QString &fingerprint) {
    auto [keys, req] = beginSignRequest(fingerprint);
    if (keys == nullptr) {
        return;
    }
    keys->signOnDevice(req, network(), m_psbt_v2_hex, toQString(getDescriptor()), fingerprint);
}

void MusigAccountController::retireSignRequests() {
    auto requests = m_sign_reqs.keys();
    m_sign_reqs.clear();
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
    retireSignRequests();
}

void MusigAccountController::cancelSigning() {
    clearSigning();
    emit sessionCleared();
}

void MusigAccountController::broadcast() {
    if (m_restart_pending || m_resetting || m_tx_hex.isEmpty() || !m_account.has_value() ||
        m_broadcast_pending) {
        return;
    }
    // stop() waits on this thread before it drops the account and before stopped()
    // lets the owner delete this controller, so both outlive the worker.
    auto *account = &*m_account.value();
    auto generation = m_signing_generation;
    auto result = std::make_shared<TxResult>();
    auto warning = std::make_shared<QString>();
    m_broadcast_pending = true;
    enqueueStorage(
        [account, result, warning, tx = m_tx_hex.toStdString()] {
            *result = account->broadcast_transaction(rust::String(tx));
            *warning = toQString(account->saved_psbt_error());
        },
        [this, result, warning, generation] {
            m_broadcast_pending = false;
            setSaveWarning(*warning);
            emit savedPsbtsChanged();
            if (generation == m_signing_generation) {
                emit broadcastDone(*result);
            }
        });
}

void MusigAccountController::onKeysSigned(ReqId req_id, const QString &psbt_v2_hex) {
    if (!m_sign_reqs.contains(req_id)) {
        return;
    }
    auto fingerprint = m_sign_reqs.take(req_id);
    // Signed on the PSBT as it was when asked: merged into the one gathered since.
    auto current = m_psbt_v2_hex.toStdString();
    auto answer = psbt_v2_hex.toStdString();
    auto merged = ::merge_signed_psbt(rust::Str(current), rust::Str(answer));
    if (!merged.is_ok) {
        emit signingFailed(toQString(merged.error), plugin::SignError::Other);
        return;
    }
    m_psbt_v2_hex = toQString(merged.psbt_v2_hex);
    persistActivePsbt();
    emit signerAnswered(fingerprint);
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
    if (!req_id.has_value() || !m_sign_reqs.contains(req_id.value())) {
        return;
    }
    m_sign_reqs.remove(req_id.value());
    if (kind == plugin::SignError::MusigSession) {
        // Pending answers belong to a session that cannot safely resume.
        retireSignRequests();
        m_restart_pending = true;
        emit signingFailed(message, kind);
        emit roundChanged(roundStatus());
        return;
    }
    emit signingFailed(message, kind);
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
    m_restart_pending = false;
    m_psbt_v2_hex = toQString(m_restart_result->psbt_v2_hex);
    m_tx_hex.clear();
    setSaveWarning(toQString(m_restart_result->save_error));
    emit savedPsbtsChanged();
    emit signingReset();
    emit roundChanged(roundStatus());
}

// The PSBT keeps the round it was at.
void MusigAccountController::onKeysCancelled(ReqId req_id) {
    if (!m_sign_reqs.contains(req_id)) {
        return;
    }
    m_sign_reqs.remove(req_id);
    emit signingCancelled();
}

void MusigAccountController::onBroadcastResult(const TxResult &result) {
    if (result.is_ok) {
        cancelSigning();
    }
    pollCoins();
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
    if (m_stopping || m_stop_requested || !m_account.has_value()) {
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
    startWorker(m_storage_thread);
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
    if (m_stop_requested && m_storage_queue.empty()) {
        stop();
    }
}

void MusigAccountController::persistActivePsbt() {
    if (!m_account.has_value() || m_saved_record_id.isEmpty()) {
        return;
    }
    auto *account = &*m_account.value();
    auto result = std::make_shared<SavedPsbtMutation>();
    enqueueStorage(
        [account, result, id = m_saved_record_id.toStdString(),
         psbt = m_psbt_v2_hex.toStdString()] { *result = account->update_saved_psbt(id, psbt); },
        [this, result] {
            setSaveWarning(toQString(result->is_ok ? result->save_error : result->error));
            emit savedPsbtsChanged();
        });
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
    enqueueStorage(
        [account, result, id = id.toStdString()] { *result = account->restore_saved_psbt(id); },
        [this, result, generation] {
            if (generation != m_signing_generation) {
                return;
            }
            if (!result->is_ok) {
                emit sessionCleared();
                emit savedPsbtFailed(toQString(result->error));
                return;
            }
            auto &entry = result->entry;
            m_saved_record_id = toQString(entry.record.id);
            m_psbt_v2_hex = toQString(entry.record.current_psbt_v2_hex);
            m_tx_hex = toQString(entry.record.finalized_tx_hex);
            m_restart_pending = entry.record.reset_pending;
            for (const auto &fingerprint : entry.record.spend.fingerprints) {
                m_spend_signers.append(toQString(fingerprint));
            }
            setSaveWarning(toQString(result->save_error));
            emit savedPsbtRestored(entry);
            loadPanel("send");
            auto status = roundStatus();
            emit roundChanged(status);
            if (m_tx_hex.isEmpty() && status.is_ok && status.round == MusigRound::Ready) {
                finalizeIfReady();
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
