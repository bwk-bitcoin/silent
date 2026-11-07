#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include "types.h"

namespace backend {

class IService : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IService() override = default;

public slots:
    virtual void raw(backend::RequestId request_id, QString operation, QByteArray payload) = 0;

    virtual void value(backend::RequestId request_id, QString operation, QJsonValue payload) = 0;

signals:
    void rawResult(backend::RequestId request_id, QByteArray payload);

    void rawEvent(QString operation, QByteArray payload);

    void valueResult(backend::RequestId request_id, QJsonValue payload);

    void valueEvent(QString operation, QJsonValue payload);

    void error(backend::RequestId request_id, QString message);
};

} // namespace backend
