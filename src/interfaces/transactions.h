#pragma once

#include <QObject>
#include <QString>
#include <interfaces/types.h>

// Caller-scoped, GUI-thread requests. Workers never access an account. Cancelling
// suppresses late replies; a broadcast already sent cannot be recalled.
class ITransactions : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~ITransactions() override = default;
    virtual auto newRequest() -> ReqId = 0;
    virtual void cancelRequest(ReqId req_id) = 0;
    virtual void finalize(ReqId req_id, const plugin::SigningRequest &request) = 0;
    virtual void broadcast(ReqId req_id, plugin::Network network, const QString &endpoint,
                           const QString &tx_hex) = 0;

signals:
    void finalized(ReqId req_id, QString psbt_v2_hex, QString tx_hex, QString txid);
    void broadcasted(ReqId req_id, QString txid);
    void error(ReqId req_id, plugin::SpendError kind, QString message);
    void cancelled(ReqId req_id);
};

Q_DECLARE_INTERFACE(ITransactions, "dev.silent.ITransactions/2.0")
