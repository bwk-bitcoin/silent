#pragma once

#include "account/ElectrumReconnect.h"
#include "interfaces/backend/types.h"
#include "interfaces/types.h"
#include "modules/shared/AccountLifecycle.h"

#include <QByteArray>
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

namespace account {
class AccountShell;
class ServiceContext;
} // namespace account

namespace musig {
class MusigServices;
} // namespace musig
class QThread;

// Owns the Rust account and the PSBT in flight. Drives MuSig2 as one
// keys()->sign() per round: Nonces, then PartialSigs, then finalize at Ready. On
// SignError::MusigSession requires an explicit restart before signing can continue.
class MusigAccountController : public AccountLifecycle {
    Q_OBJECT

public:
    enum class ElectrumState { Disconnected, Connecting, Connected, Disconnecting };

    MusigAccountController(const QString &account, MusigAccountWidget *widget);
    ~MusigAccountController() override;
    void init(const QString &account);
    void loadPanels();
    // The page the account's panels show in, null for none: the panels are built in it.
    void setShell(account::AccountShell *shell);
    [[nodiscard]] auto shell() const -> account::AccountShell *;
    [[nodiscard]] auto screen(const QString &screen) const -> std::optional<qontrol::Screen *>;
    auto getAccount() -> std::optional<rust::Box<MusigAccount>> &;
    auto network() -> Network override;
    // A new taproot address, the only kind it receives on. Empty on failure.
    auto newAddress() -> QString;
    auto getDescriptor() -> rust::String;
    auto cosignerFingerprints() -> QStringList;
    // Electrum only.
    auto endpoints() -> QList<Endpoint> override;
    auto endpointUrl(Endpoint endpoint) -> QString override;
    [[nodiscard]] auto verifiesCertificate() const -> bool override;
    [[nodiscard]] auto isEndpointRunning(Endpoint endpoint) const -> bool override;
    // Electrum is running and no endpoint change is under way: a read may go online.
    [[nodiscard]] auto electrumUsable() const -> bool;
    // A signing request, a finalization, a reset or a broadcast runs.
    [[nodiscard]] auto endpointChangeRefusal() const -> QString override;
    // What Settings shows below the endpoints: the descriptor, the cosigners and Delete.
    auto settingsExtras() -> QWidget *;
    [[nodiscard]] auto services() const -> account::ServiceContext *;
    auto module() -> IModule * override;
    // stop() waits for the worker before it drops the account.
    void startWorker(QThread *worker) override;
    void requestEndpointTip(uint64_t req_id, Endpoint endpoint) override;
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
    // Saves the PSBT `text` holds (base64, or binary) ready to sign, or merges it into
    // the saved PSBT of its transaction; psbtImported() answers.
    void importSavedPsbt(const QByteArray &text);

signals:
    // The account's coins and balance, polled after they may have changed.
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);
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
    // exportTransaction() answered: the PSBTv2 restricted to its spend path, its change
    // registered.
    void transactionExported(QByteArray psbt);
    void exportFailed(QString message);
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
    void savedPsbtsListed(SavedPsbtList result);
    void savedPsbtRestored(SavedPsbtEntry entry);
    void savedPsbtsChanged();
    void saveWarningChanged(QString message);
    void savedPsbtFailed(QString message);
    void savedPsbtWarningChanged(QString message);
    void savedPsbtDeleteConfirmationNeeded(QString id);
    void psbtImported(SavedPsbtImport result);
    void sessionCleared();

public slots:
    void loadPanel(const QString &name);
    void pollCoins();
    void handleNotification(Notification notif);
    void onNotificationReceived(uint64_t generation, Notification notif);
    // A lost Electrum connection starts again.
    void onElectrumReconnect();
    // The reconnect gave up: the user is told, the connection stays down.
    void onElectrumGaveUp(int losses);
    // A reconnect held back by a settings test or save runs.
    void onSettingsIdle();
    void stop();
    // Takes `psbt`, ITransaction::finalize() of the crafted `tx`, as the transaction in
    // flight: registers its change, restricts it to `spend` and saves it before round 1.
    // A change index another adoption took refuses it: `tx` is to be crafted again.
    void adoptTransaction(const backend::TransactionTemplate &tx, const MusigSpendLeaf &spend,
                          const QByteArray &psbt);
    // Hands `psbt`, ITransaction::finalize() of the crafted `tx`, out with no signing
    // session: registers its change and restricts it to `spend`, saving nothing. A change
    // index another adoption or export took refuses it: `tx` is to be crafted again.
    void exportTransaction(const backend::TransactionTemplate &tx, const MusigSpendLeaf &spend,
                           const QByteArray &psbt);
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
    void onKeysSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onKeysCancelled(ReqId req_id);
    void onNoncesPrepared(ReqId req_id, const QString &fingerprint);
    void onWorkerFinished();
    void onEndpointFinished();
    void onAccountLoaded();
    void onRestartFinished();
    void onStorageFinished();
    void onSavedPsbtDeleteConfirmed(const QString &id);
    void onSavedPsbtDeleteCancelled(const QString &id);
    // The first funds read answered: the panels reading them are built.
    void onFundsAnswered();
    void psbtClicked();
    void historyClicked();
    void coinsClicked();
    void sendClicked();
    void receiveClicked();
    void settingsClicked();

protected:
    friend class musig::MusigServices;

    // Broadcasts `transaction` through the storage queue, behind the saved progress writes,
    // then answers `done`. The finalized transaction in flight, once accepted, ends its
    // signing session. Refused (false, `done` never called) while a reset, a restart, an
    // endpoint change or another broadcast runs.
    auto startBroadcast(const QByteArray &transaction,
                        std::function<void(const backend::BroadcastResult &)> done) -> bool;
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
    void changeEndpoint(uint64_t req_id, Endpoint endpoint, bool connected, bool retry = false);
    void startEndpointChange();
    void connectElectrum(bool retry) override;
    void electrumProbeFailed(bool retry, const EndpointResult &result) override;

private:
    MusigAccountWidget *m_widget = nullptr;
    QPointer<account::AccountShell> m_shell;
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
    bool m_endpoint_connect = false;
};
