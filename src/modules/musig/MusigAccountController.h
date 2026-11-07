#pragma once

#include "account/AccountBackend.h"
#include "account/ElectrumReconnect.h"
#include "account/PanelCollection.h"
#include "interfaces/types.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <Qontrol>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <silent.h>

class IKeys;
class MusigAccountWidget;

namespace musig {
class MusigServices;
} // namespace musig
class QThread;

// Owns the Rust account and the PSBT in flight. Drives MuSig2 as one
// keys()->sign() per round: Nonces, then PartialSigs, then finalize at Ready. On
// SignError::MusigSession requires an explicit restart before signing can continue.
class MusigAccountController : public AccountBackend {
    Q_OBJECT

public:
    enum class ElectrumState { Disconnected, Connecting, Connected, Disconnecting };

    MusigAccountController(const QString &account, MusigAccountWidget *widget);
    ~MusigAccountController() override;
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
    // The descriptor, the cosigners and Delete.
    auto settingsExtras() -> QWidget * override;
    [[nodiscard]] auto services() const -> account::ServiceContext * override;
    auto module() -> IModule * override;
    auto simulateTx(TransactionTemplate tx) -> TransactionSimulation override;
    // The spend path, Prepare and the MuSig2 rounds.
    auto sendSigning() -> account::SendSigning * override;
    // stop() waits for the worker before it drops the account.
    void startWorker(QThread *worker) override;
    void requestTxDetails(uint64_t req_id, const QString &txid) override;
    void editTxLabel(uint64_t req_id, const QString &txid, const QString &label) override;
    void editOutpointLabel(uint64_t req_id, const QString &outpoint, const QString &label) override;
    void requestEndpointTip(uint64_t req_id, Endpoint endpoint) override;
    void testAndSaveEndpoint(uint64_t req_id, Endpoint endpoint, const QString &url,
                             bool verify_certificate) override;
    void setEndpointConnected(uint64_t req_id, Endpoint endpoint, bool connected) override;
    auto electrumUrl() -> QString;
    [[nodiscard]] auto electrumState() const -> ElectrumState;
    [[nodiscard]] auto roundStatus() const -> MusigRoundStatus;
    [[nodiscard]] auto psbtInFlight() const -> QString;
    // The finalized transaction hex, empty until the PSBT in flight is finalized.
    // The keys asked to sign now, lowercased fingerprints.
    [[nodiscard]] auto signingKeys() const -> QSet<QString>;
    // The keys preparing a nonce-cache session now, lowercased fingerprints.
    [[nodiscard]] auto preparingKeys() const -> QSet<QString>;
    // Whether a nonce session was durably taken from cache for this attempt: Reset
    // exists from here on, even when the request it rode on failed afterward.
    [[nodiscard]] auto noncesAllocated() const -> bool;
    [[nodiscard]] auto isRestarting() const -> bool;
    [[nodiscard]] auto restartPending() const -> bool;
    [[nodiscard]] auto finalizedTx() const -> QString;
    // The fingerprints of the spend path the prepared transaction spends with, empty
    // with no transaction prepared.
    [[nodiscard]] auto spendSigners() const -> QStringList;
    auto requestSigningPlan() -> uint64_t;
    // MusigAccount::spend_leaves for the selected coins.
    auto spendLeaves(const QStringList &outpoints) -> MusigSpendLeaves;
    [[nodiscard]] auto saveWarning() const -> QString;
    [[nodiscard]] auto activeRecordId() const -> QString;
    void listSavedPsbts(bool include_confirmed);
    void restoreSavedPsbt(const QString &id);
    void deleteSavedPsbt(const QString &id);
    void labelSavedPsbt(const QString &id, const QString &label);
    void retrySavedPsbts();

signals:
    void signingPlanReady(uint64_t req_id, SigningPlan plan);
    void electrumConnected(QString address);
    void electrumDisconnected();
    // The Electrum URL was saved.
    void electrumUrlChanged();
    void notificationReceived(uint64_t generation, Notification notif);
    // A prepared PSBT entered round 1.
    void prepared(QString psbt_v2_hex, QString txid);
    void sessionLoading();
    void prepareFailed(QString message);
    void roundChanged(MusigRoundStatus status);
    void signingReset();
    void signingFailed(QString message, plugin::SignError kind);
    // The key `fingerprint` answered its request; roundChanged() follows.
    void signerAnswered(QString fingerprint);
    void signingCancelled();
    // Finalized at Ready.
    void transactionReady(QString tx_hex);
    // finalize_psbt failed at Ready.
    void finalizeFailed(QString message);
    void broadcastDone(TxResult result);
    void savedPsbtsListed(SavedPsbtList result);
    void savedPsbtRestored(SavedPsbtEntry entry);
    void savedPsbtsChanged();
    void saveWarningChanged(QString message);
    void savedPsbtFailed(QString message);
    void savedPsbtWarningChanged(QString message);
    void savedPsbtDeleteConfirmationNeeded(QString id);
    void sessionCleared();

public slots:
    void loadPanel(const QString &name);
    void pollCoins();
    void handleNotification(Notification notif);
    void onNotificationReceived(uint64_t generation, Notification notif);
    void updateCoinLabel(const QString &outpoint, const QString &label) override;
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
    // Fills the key's nonce cache with one session, taking none for the transaction.
    void prepareNonces(const QString &fingerprint);
    void cancelSigning();
    void restartRounds();
    void broadcast();
    void onKeysSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onKeysCancelled(ReqId req_id);
    void onNoncesPrepared(ReqId req_id, const QString &fingerprint);
    void onBroadcastResult(const TxResult &result);
    void onWorkerFinished();
    void onEndpointFinished();
    void onAccountLoaded();
    void onRestartFinished();
    void onStorageFinished();
    void onSavedPsbtDeleteConfirmed(const QString &id);
    void onSavedPsbtDeleteCancelled(const QString &id);
    void psbtClicked();
    void historyClicked();
    void coinsClicked();
    void sendClicked();
    void receiveClicked();
    void settingsClicked();

protected:
    // Broadcasts through the storage queue, behind the saved progress writes.
    friend class musig::MusigServices;

    void startAcceptedWorker(QThread *worker);
    // Listens to the keys module and allocates the request of a round's signature, null
    // when no round can be signed now.
    // A request of `fingerprint`'s key allocated, null when none can be sent now (no
    // PSBT, or that key already asked). Every key may be asked at once.
    [[nodiscard]] auto beginSignRequest(const QString &fingerprint) -> std::pair<IKeys *, ReqId>;
    void finalizeIfReady();
    void retireSignRequests();
    // Re-reads the active attempt from Rust and refreshes everything that follows it.
    void refreshAttempt();
    void clearSigning();
    void enqueueStorage(std::function<void()> work, std::function<void()> done);
    void startStorage();
    void applyAttempt(const MusigAttemptSnapshot &snapshot);
    void setSaveWarning(const QString &message);
    void refreshActiveWarning();
    void completeSavedPsbtDeletion(const QString &id, const SavedPsbtMutation &result);
    void listenNotifications();
    void changeEndpoint(uint64_t req_id, Endpoint endpoint, const QString &url,
                        bool verify_certificate, bool test, bool connected, bool retry = false);
    void startEndpointChange();

private:
    account::PanelCollection m_panels{this};
    MusigAccountWidget *m_widget = nullptr;
    account::ServiceContext *m_services = nullptr;
    std::optional<rust::Box<MusigAccount>> m_account = std::nullopt;
    QThread *m_notif_thread = nullptr;
    uint64_t m_notif_generation = 0;
    QList<QThread *> m_workers;
    ElectrumState m_electrum_state = ElectrumState::Disconnected;
    account::ElectrumReconnect *m_reconnect = new account::ElectrumReconnect(this);
    QString m_psbt_v2_hex;
    QString m_tx_hex;
    QString m_saved_record_id;
    QString m_save_warning;
    enum class DeletionState { Checking, AwaitingConfirmation, Deleting };
    QHash<QString, DeletionState> m_deletions;

    struct StorageWork {
        std::function<void()> work;
        std::function<void()> done;
    };

    std::deque<StorageWork> m_storage_queue;
    QThread *m_storage_thread = nullptr;
    bool m_finalizing = false;
    bool m_broadcast_pending = false;
    // The signers of the spend path of the prepared transaction.
    QStringList m_spend_signers;
    // Sign requests in flight, each one key's, by request id. Answers are merged into
    // the PSBT as it is by then, so keys sign at once.
    QHash<ReqId, QString> m_sign_reqs;
    // Nonce-cache preparations in flight, by request id.
    QHash<ReqId, QString> m_prepare_reqs;
    QSet<ReqId> m_accepting_reqs;
    // A nonce session was durably taken from cache for this attempt.
    bool m_nonces_allocated = false;
    uint64_t m_next_plan_id = 0;
    QPointer<IKeys> m_signing_keys;
    bool m_resetting = false;
    std::shared_ptr<MusigAttemptSnapshot> m_restart_result;
    QString m_restart_source;
    bool m_restart_pending = false;
    std::uint64_t m_signing_generation = 0;
    std::uint64_t m_restart_generation = 0;
    bool m_init = false;
    bool m_panels_requested = false;
    QString m_requested_panel = QStringLiteral("history");
    std::shared_ptr<std::optional<rust::Box<MusigAccount>>> m_loading_account;
    bool m_stopping = false;
    bool m_stop_requested = false;
    std::shared_ptr<std::optional<rust::Box<MusigAccount>>> m_endpoint_account;
    std::shared_ptr<EndpointResult> m_endpoint_result;
    std::shared_ptr<bool> m_endpoint_replaced;
    Endpoint m_endpoint = Endpoint::Electrum;
    bool m_endpoint_busy = false;
    bool m_endpoint_retry = false;
    bool m_reconnect_pending = false;
    QList<Notification> m_endpoint_notifications;
    QString m_endpoint_url;
    bool m_endpoint_verify = true;
    bool m_endpoint_test = false;
    bool m_endpoint_connect = false;
};
