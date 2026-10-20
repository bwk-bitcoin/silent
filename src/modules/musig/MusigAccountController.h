#pragma once

#include "account/AccountBackend.h"
#include "account/ElectrumReconnect.h"
#include "interfaces/types.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <Qontrol>
#include <cstdint>
#include <memory>
#include <optional>
#include <silent.h>

class IKeys;
class MusigAccountWidget;
class QThread;

// Owns the Rust account and the PSBT in flight. Drives MuSig2 as one
// keys()->sign() per round: Nonces, then PartialSigs, then finalize at Ready. On
// SignError::MusigSession it retires Ledger allocations on a worker, then drops
// every nonce and partial sig and starts a fresh attempt over the prepared PSBT.
class MusigAccountController : public AccountBackend {
    Q_OBJECT

public:
    MusigAccountController(const QString &account, MusigAccountWidget *widget);
    ~MusigAccountController() override = default;
    void init(const QString &account);
    void loadPanels();
    auto screen(const QString &screen) -> std::optional<qontrol::Screen *>;
    auto getAccount() -> std::optional<rust::Box<MusigAccount>> &;
    auto getCoins() -> rust::Vec<RustCoin> override;
    auto getPaymentHistory() -> rust::Vec<RustTx> override;
    auto network() -> Network override;
    // A taproot address only.
    auto addressKinds() -> QList<AddressKind> override;
    auto newAddress(AddressKind kind) -> QString override;
    auto getDescriptor() -> rust::String;
    auto cosignerFingerprints() -> QStringList;
    auto addressVerifyInfo(const QString &address) -> AddressVerifyInfo override;
    // Electrum only.
    auto endpoints() -> QList<Endpoint> override;
    auto endpointUrl(Endpoint endpoint) -> QString override;
    [[nodiscard]] auto verifiesCertificate() const -> bool override;
    [[nodiscard]] auto isEndpointRunning(Endpoint endpoint) const -> bool override;
    auto startEndpoint(Endpoint endpoint) -> bool override;
    void stopEndpoint(Endpoint endpoint) override;
    [[nodiscard]] auto networkEditable() const -> bool override;
    // Saves the Electrum url and restarts the listener on it; the network is fixed.
    auto saveSettings(const QHash<Endpoint, QString> &urls, Network network) -> QString override;
    // The descriptor, the cosigners and Delete.
    auto settingsExtras() -> QWidget * override;
    auto module() -> IModule * override;
    auto simulateTx(TransactionTemplate tx) -> TransactionSimulation override;
    // The spend path, Prepare and the MuSig2 rounds.
    auto sendSigning() -> account::SendSigning * override;
    // stop() waits for the worker before it drops the account.
    void startWorker(QThread *worker) override;
    auto electrumUrl() -> QString;
    [[nodiscard]] auto roundStatus() const -> MusigRoundStatus;
    [[nodiscard]] auto psbtInFlight() const -> QString;
    // The finalized transaction hex, empty until the PSBT in flight is finalized.
    // The keys asked to sign now, lowercased fingerprints.
    [[nodiscard]] auto signingKeys() const -> QSet<QString>;
    [[nodiscard]] auto isRestarting() const -> bool;
    [[nodiscard]] auto restartPending() const -> bool;
    [[nodiscard]] auto finalizedTx() const -> QString;
    // The fingerprints of the spend path the prepared transaction spends with, empty
    // with no transaction prepared.
    [[nodiscard]] auto spendSigners() const -> QStringList;
    // MusigAccount::spend_leaves for the selected coins.
    auto spendLeaves(const QStringList &outpoints) -> MusigSpendLeaves;

signals:
    void electrumConnected(QString address);
    void electrumDisconnected();
    // The Electrum URL was saved.
    void electrumUrlChanged();
    void notificationReceived(Notification notif);
    // A prepared PSBT entered round 1.
    void prepared(QString psbt_v2_hex, QString txid);
    void prepareFailed(QString message);
    void roundChanged(MusigRoundStatus status);
    void signingFailed(QString message, plugin::SignError kind);
    // The key `fingerprint` answered its request; roundChanged() follows.
    void signerAnswered(QString fingerprint);
    void signingCancelled();
    // Finalized at Ready.
    void transactionReady(QString tx_hex);
    // finalize_psbt failed at Ready.
    void finalizeFailed(QString message);
    void broadcastDone(TxResult result);
    // Background teardown done, the owner may delete the widget.
    void stopped();

public slots:
    void loadPanel(const QString &name);
    void pollCoins();
    void handleNotification(Notification notif);
    void updateCoinLabel(const QString &outpoint, const QString &label) override;
    auto setElectrumUrl(const QString &url) -> MusigElectrumUrlResult;
    void startElectrum();
    void stopElectrum();
    // A lost Electrum connection starts again.
    void onElectrumReconnect();
    // The reconnect gave up: the user is told, the connection stays down.
    void onElectrumGaveUp(int losses);
    void stop();
    // Prepares, then restricts the PSBT to `spend` before round 1.
    void prepareTransaction(TransactionTemplate tx, MusigSpendLeaf spend);
    // keys()->sign() with the key `fingerprint` alone, for the current round of the PSBT
    // in flight.
    void signRound(const QString &fingerprint);
    // The same over the key's connected USB device, with no window: the caller shows
    // the wait.
    void signRoundOnDevice(const QString &fingerprint);
    void cancelSigning();
    void restartRounds();
    void broadcast();
    void onKeysSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onKeysCancelled(ReqId req_id);
    void onBroadcastResult(const TxResult &result);
    void onBroadcastThreadFinished();
    void onWorkerFinished();
    void onRestartFinished();
    void historyClicked();
    void coinsClicked();
    void sendClicked();
    void receiveClicked();
    void settingsClicked();

protected:
    // Listens to the keys module and allocates the request of a round's signature, null
    // when no round can be signed now.
    // A request of `fingerprint`'s key allocated, null when none can be sent now (no
    // PSBT, or that key already asked). Every key may be asked at once.
    [[nodiscard]] auto beginSignRequest(const QString &fingerprint) -> std::pair<IKeys *, ReqId>;
    void finalizeIfReady();
    void retireSignRequests();

private:
    QPointer<qontrol::Panel> m_current_panel;
    QHash<QString, qontrol::Panel *> m_panels;
    MusigAccountWidget *m_widget = nullptr;
    std::optional<rust::Box<MusigAccount>> m_account = std::nullopt;
    QThread *m_notif_thread = nullptr;
    QThread *m_broadcast_thread = nullptr;
    QList<QThread *> m_workers;
    bool m_electrum_running = false;
    account::ElectrumReconnect *m_reconnect = new account::ElectrumReconnect(this);
    QString m_prepared_psbt_v2_hex;
    QString m_psbt_v2_hex;
    QString m_tx_hex;
    // The signers of the spend path of the prepared transaction.
    QStringList m_spend_signers;
    // Sign requests in flight, each one key's, by request id. Answers are merged into
    // the PSBT as it is by then, so keys sign at once.
    QHash<ReqId, QString> m_sign_reqs;
    QPointer<IKeys> m_signing_keys;
    QThread *m_restart_thread = nullptr;
    std::shared_ptr<SignResult> m_restart_result;
    QString m_restart_source;
    QString m_restart_message;
    bool m_restart_pending = false;
    std::uint64_t m_signing_generation = 0;
    std::uint64_t m_restart_generation = 0;
    bool m_init = false;
    bool m_stopping = false;
};
