#pragma once

#include <optional>

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include <interfaces/types.h>

class IAccount : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IAccount() override = default;

    [[nodiscard]] virtual bool implemented() const = 0;
    virtual ReqId name() = 0;
    virtual ReqId info() = 0;
    virtual ReqId coins() = 0;
    virtual ReqId balance() = 0;
    virtual ReqId newReceiveAddress() = 0;
    virtual ReqId newChangeAddress() = 0;
    // The account's full descriptor, answered on descriptor(ReqId, QString). For an sp
    // account too: whoever holds it can scan the account.
    virtual ReqId descriptor() = 0;
    virtual ReqId raw(const QByteArray &request) = 0;

signals:
    void name(ReqId req_id, QString name);
    void info(ReqId req_id, QMap<QString, QString> info);
    void coins(ReqId req_id, QList<plugin::Coin> coins);
    void balance(ReqId req_id, plugin::Balance balance);
    void receiveAddress(ReqId req_id, QString address);
    void changeAddress(ReqId req_id, QString address);
    void descriptor(ReqId req_id, QString descriptor);
    void raw(ReqId req_id, QByteArray response);
    void error(std::optional<ReqId> req_id, QString message);
};

Q_DECLARE_INTERFACE(IAccount, "dev.silent.IAccount/1.0")
