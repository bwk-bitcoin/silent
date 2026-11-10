#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include "types.h"

namespace backend {

// The base of every account UI service: the extension requests and their answers, and the
// error() any request may answer. Every request is a slot taking a RequestId: it returns at
// once and is answered once, by its own signal or by error(), possibly during the call. An
// unsupported request answers error(), never nothing.
class IService : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IService() override = default;

public slots:
    // Runs the extension `operation` (named by its provider, "silent." for the built-in ones)
    // on bytes it defines, answered by rawResult(). An unknown operation answers error().
    virtual void raw(backend::RequestId request_id, QString operation, QByteArray payload) = 0;

    // value() alike on Qt JSON, answered by valueResult().
    virtual void value(backend::RequestId request_id, QString operation, QJsonValue payload) = 0;

signals:
    // Answers raw().
    void rawResult(backend::RequestId request_id, QByteArray payload);

    // An update of `operation` no request asked for, its payload as raw() defines it.
    void rawEvent(QString operation, QByteArray payload);

    // Answers value().
    void valueResult(backend::RequestId request_id, QJsonValue payload);

    // rawEvent() alike, its payload as value() defines it.
    void valueEvent(QString operation, QJsonValue payload);

    // Answers the request `request_id` that failed, `message` saying why in words for the
    // user. Request id 0 for a call taking none (ITransaction::finalize()).
    void error(backend::RequestId request_id, QString message);
};

} // namespace backend
