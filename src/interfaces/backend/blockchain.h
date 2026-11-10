#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

#include "service.h"
#include "types.h"

namespace backend {

// What an account finds on chain: its scans and connections, coins, balance, history and
// transaction details, and the broadcast of its transactions. A descriptor-based account's
// service is given an AccountWatch and reports what pays it (ownedOutputs(), usedIndexes()).
class IBlockchain : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~IBlockchain() override = default;

    // Changes with stateChanged().
    [[nodiscard]] virtual auto state() const -> BlockchainState = 0;

    // Every coin of the account, spent ones included. Changes with coinsChanged().
    [[nodiscard]] virtual auto coins() const -> QList<Coin> = 0;

    // Changes with balanceChanged().
    [[nodiscard]] virtual auto balance() const -> Balance = 0;

    // Every transaction of the account; a built-in account lists what its scan found newest
    // first, the unconfirmed ones on top. Changes with historyChanged().
    [[nodiscard]] virtual auto history() const -> QList<Transaction> = 0;

    // The details of the hex txid `txid` last fetched by fetchTransactionDetails(),
    // std::nullopt until then. Changes with transactionDetailsChanged().
    [[nodiscard]] virtual auto transactionDetails(const QString &txid) const
        -> std::optional<TransactionDetails> = 0;

    // Every output paying the AccountWatch descriptors the service was given, spent ones
    // included. Empty without a watch. Changes with coinsChanged().
    [[nodiscard]] virtual auto ownedOutputs() const -> QList<OwnedOutput> = 0;

    // How far each branch of those descriptors is used, one entry per branch. Empty without
    // a watch. Changes with coinsChanged().
    [[nodiscard]] virtual auto usedIndexes() const -> QList<UsedIndex> = 0;

public slots:
    // Connects to every server of state(), acknowledged by completed().
    virtual void start(backend::RequestId request_id) = 0;
    // Disconnects from every server, acknowledged by completed().
    virtual void stop(backend::RequestId request_id) = 0;

    // Scans the chain again from block height `from_height`, unset for where the service
    // starts scanning, acknowledged by completed(). No built-in account supports it.
    virtual void rescan(backend::RequestId request_id, std::optional<quint64> from_height) = 0;

    // Connects to, or disconnects from with `connected` false, the server of Connection::id
    // `connection_id`, acknowledged by completed().
    virtual void setConnected(backend::RequestId request_id, QString connection_id,
                              bool connected) = 0;

    // Asks the server of Connection::id `connection_id` its chain height, which its
    // Connection::tip then holds, acknowledged by completed().
    virtual void refreshTip(backend::RequestId request_id, QString connection_id) = 0;

    // Fetches the details of the hex txid `txid`, answered by transactionDetailsFetched()
    // and kept for transactionDetails(). A built-in account looks up what it lacks only
    // while its Electrum server is connected, saying what is missing in its warnings.
    virtual void fetchTransactionDetails(backend::RequestId request_id, QString txid) = 0;

    // Sends the consensus-encoded `transaction` to the network, answered by
    // broadcastFinished(), whatever its outcome.
    virtual void broadcast(backend::RequestId request_id, QByteArray transaction) = 0;

    // Watches the branch `keychain` of the descriptor at `descriptor` in AccountWatch up to
    // derivation index `index`, past the gap limit if needed. Acknowledged by completed().
    virtual void reveal(backend::RequestId request_id, quint32 descriptor, quint32 keychain,
                        quint32 index) = 0;

signals:
    // state() changed.
    void stateChanged();
    // coins(), and with them ownedOutputs() and usedIndexes(), changed.
    void coinsChanged();
    // balance() changed.
    void balanceChanged();
    // history() changed.
    void historyChanged();
    // transactionDetails() of the hex txid `txid` changed.
    void transactionDetailsChanged(QString txid);

    // Acknowledges start, stop, rescan, connection, tip or reveal requests.
    // Current state is read through state().
    void completed(backend::RequestId request_id);

    // Answers fetchTransactionDetails().
    void transactionDetailsFetched(backend::RequestId request_id,
                                   backend::TransactionDetails details);

    // Answers broadcast(): a refused or undelivered transaction is a result, not an error().
    void broadcastFinished(backend::RequestId request_id, backend::BroadcastResult result);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IBlockchain, "dev.silent.backend.IBlockchain/1.0")
