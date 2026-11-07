#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

#include "service.h"
#include "types.h"

namespace backend {

class IBlockchain : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~IBlockchain() override = default;

    [[nodiscard]] virtual auto state() const -> BlockchainState = 0;

    [[nodiscard]] virtual auto coins() const -> QList<Coin> = 0;

    [[nodiscard]] virtual auto balance() const -> Balance = 0;

    [[nodiscard]] virtual auto history() const -> QList<Transaction> = 0;

    [[nodiscard]] virtual auto transactionDetails(const QString &txid) const
        -> std::optional<TransactionDetails> = 0;

public slots:
    virtual void start(backend::RequestId request_id) = 0;
    virtual void stop(backend::RequestId request_id) = 0;

    virtual void rescan(backend::RequestId request_id, std::optional<quint64> from_height) = 0;

    virtual void setConnected(backend::RequestId request_id, QString connection_id,
                              bool connected) = 0;

    virtual void refreshTip(backend::RequestId request_id, QString connection_id) = 0;

    virtual void fetchTransactionDetails(backend::RequestId request_id, QString txid) = 0;

    virtual void broadcast(backend::RequestId request_id, QByteArray transaction) = 0;

signals:
    void stateChanged();
    void coinsChanged();
    void balanceChanged();
    void historyChanged();
    void transactionDetailsChanged(QString txid);

    // Acknowledges start, stop, rescan, connection or tip requests.
    // Current state is read through state().
    void completed(backend::RequestId request_id);

    void transactionDetailsFetched(backend::RequestId request_id,
                                   backend::TransactionDetails details);

    void broadcastFinished(backend::RequestId request_id, backend::BroadcastResult result);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IBlockchain, "dev.silent.backend.IBlockchain/1.0")
