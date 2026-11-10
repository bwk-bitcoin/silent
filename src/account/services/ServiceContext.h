#pragma once

#include "interfaces/backend/types.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <optional>

class QTimer;

namespace account {

// What one account's UI services share: the read models its adapters publish, and the
// requests they forward. Getters read the published models under a short lock, never
// the account itself; the account side publishes them as its state changes, whether
// or not anyone listens, so polling and notified consumers read the same data.
// Requests answer through the completion signals, keyed by the token they were given,
// possibly before the request call returns. GUI thread only, getters included: the
// adapters reach the context through a QPointer, which another thread cannot read safely.
class ServiceContext : public QObject {
    Q_OBJECT

public:
    // One read of the coins, balance and history.
    struct Funds {
        QList<backend::Coin> coins;
        backend::Balance balance;
        QList<backend::Transaction> history;
        // Every coin's funding transaction is in the history.
        bool historyComplete = true;
    };

    struct Receiving {
        QList<backend::AddressKind> kinds;
        QList<backend::Address> addresses;
        QHash<QString, backend::AddressVerification> verification;
    };

    ServiceContext(QString account, QObject *parent);

    [[nodiscard]] auto account() const -> QString;
    [[nodiscard]] auto network() const -> backend::Network;
    [[nodiscard]] auto coins() const -> QList<backend::Coin>;
    [[nodiscard]] auto balance() const -> backend::Balance;
    [[nodiscard]] auto history() const -> QList<backend::Transaction>;
    [[nodiscard]] auto transactionDetails(const QString &txid) const
        -> std::optional<backend::TransactionDetails>;
    // What the details of `txid` leave out (extension::TRANSACTION_EXTRAS), once fetched.
    [[nodiscard]] auto transactionExtras(const QString &txid) const -> std::optional<QJsonObject>;
    [[nodiscard]] auto labels() const -> QList<backend::Label>;
    [[nodiscard]] auto label(const backend::LabelKey &key) const -> QString;
    [[nodiscard]] auto addressKinds() const -> QList<backend::AddressKind>;
    [[nodiscard]] auto addresses() const -> QList<backend::Address>;
    [[nodiscard]] auto addressVerification(const QString &address) const
        -> std::optional<backend::AddressVerification>;
    [[nodiscard]] auto state() const -> backend::BlockchainState;
    [[nodiscard]] auto settings() const -> backend::Settings;
    // An endpoint operation runs (extension::ENDPOINTS_BUSY).
    [[nodiscard]] auto endpointsBusy() const -> bool;
    // The first funds read answered (fundsAnswered()).
    [[nodiscard]] auto fundsKnown() const -> bool;

    // Local and pure: no selection, persistence or network.
    virtual auto craft(const backend::TransactionTemplate &transaction) -> backend::CraftResult = 0;
    virtual auto finalize(const backend::TransactionTemplate &transaction)
        -> std::optional<QByteArray> = 0;

    virtual void selectCoins(backend::RequestId token,
                             backend::TransactionTemplate transaction) = 0;
    virtual void newAddress(backend::RequestId token, QString kind) = 0;
    virtual void setLabel(backend::RequestId token, backend::LabelKey key, QString text) = 0;
    virtual void fetchTransactionDetails(backend::RequestId token, QString txid) = 0;
    // Connects, then disconnects with `start` false, every connection in turn.
    virtual void connectAll(backend::RequestId token, bool start) = 0;
    virtual void setConnected(backend::RequestId token, QString connection, bool connected) = 0;
    virtual void refreshTip(backend::RequestId token, QString connection) = 0;
    virtual void testEndpoint(backend::RequestId token, backend::Endpoint endpoint) = 0;
    virtual void saveSettings(backend::RequestId token, backend::Settings settings) = 0;
    // What the server of `endpoint` reports it serves (extension::ENDPOINT_CAPABILITIES):
    // answers valueAnswered().
    virtual void endpointCapabilities(backend::RequestId token, QString endpoint) = 0;
    virtual void broadcast(backend::RequestId token, QByteArray transaction) = 0;
    // What the account's backup holds and what opens it: empty for an account with none.
    [[nodiscard]] virtual auto backupDescription() const -> backend::BackupDescription;
    // The account's backup, encrypted to its keys: answers backupCreated(). An account
    // with none to give answers failed().
    virtual void backup(backend::RequestId token);
    // What the backup `backup` holds, an encrypted one opened with one of `xpubs` or with
    // `mnemonic`: answers backupInspected(), or backupLocked() when none opens it.
    virtual void inspectBackup(backend::RequestId token, const QByteArray &backup,
                               const QStringList &xpubs, const QByteArray &mnemonic);

signals:
    void coinsChanged();
    void balanceChanged();
    void historyChanged();
    void transactionDetailsChanged(QString txid);
    void labelsChanged();
    void addressesChanged();
    void stateChanged();
    void settingsChanged();
    void endpointsBusyChanged(bool busy);

    void coinsSelected(backend::RequestId token, backend::CraftResult result);
    void addressCreated(backend::RequestId token, backend::Address address);
    void labelSaved(backend::RequestId token);
    void transactionDetailsFetched(backend::RequestId token, backend::TransactionDetails details);
    // A connection, tip or settings request is done.
    void completed(backend::RequestId token);
    void endpointTested(backend::RequestId token);
    void broadcastFinished(backend::RequestId token, backend::BroadcastResult result);
    void valueAnswered(backend::RequestId token, QJsonValue payload);
    void backupCreated(backend::RequestId token, QByteArray backup, QStringList excluded);
    void backupInspected(backend::RequestId token, backend::BackupInfo info);
    void backupLocked(backend::RequestId token, QStringList derivation_paths);
    // finalize() built nothing, for `reason`.
    void finalizeFailed(QString reason);
    // The first funds read answered, failed or could not start: the views open on it.
    void fundsAnswered();
    void failed(backend::RequestId token, QString message);

public slots:
    // Reads the funds again and publishes them. While their history lags their coins,
    // it reads them again every second for a while, notified or not.
    void onFundsChanged();
    void onLabelsChanged();
    void onReceivingChanged();
    void onReconcileTimeout();

protected:
    // Starts reading the account's funds, off the GUI thread for an account: the read
    // answers fundsRead() on the GUI thread, possibly before this returns. False when the
    // account cannot be read now (closed, busy).
    virtual auto startFundsRead() -> bool = 0;
    // The account's current state, nullopt while it cannot be read (closed, busy).
    virtual auto readLabels() -> std::optional<QList<backend::Label>> = 0;
    virtual auto readReceiving() -> std::optional<Receiving> = 0;

    // The answer of startFundsRead(), nullopt when the read failed.
    void fundsRead(const std::optional<Funds> &funds);
    void publishFunds(const Funds &funds);
    void publishNetwork(backend::Network network);
    void publishDetails(const backend::TransactionDetails &details, const QJsonObject &extras);
    void publishState(const backend::BlockchainState &state);
    void publishSettings(const backend::Settings &settings);
    void publishEndpointsBusy(bool busy);
    // Reads the funds, or once more after the read in flight.
    void reconcileFunds();
    // While the history lags the coins, the funds are read again a second later.
    void scheduleReconcile();
    void markFundsKnown();

private:
    QString m_account;
    mutable QMutex m_mutex;
    backend::Network m_network = backend::Network::Bitcoin;
    Funds m_funds;
    QHash<QString, backend::TransactionDetails> m_details;
    QHash<QString, QJsonObject> m_extras;
    QList<backend::Label> m_labels;
    Receiving m_receiving;
    backend::BlockchainState m_state;
    backend::Settings m_settings;
    bool m_endpoints_busy = false;
    QTimer *m_reconcile = nullptr;
    int m_reconcile_attempts = 0;
    bool m_history_pending = false;
    bool m_funds_reading = false;
    bool m_funds_known = false;
    // The funds changed while a read was in flight: they are read again once it answers.
    bool m_funds_again = false;
};

} // namespace account
