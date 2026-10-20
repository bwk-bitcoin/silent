#pragma once

#include <memory>
#include <optional>

#include <QObject>
#include <QPointer>
#include <QString>

#include <interfaces/types.h>
#include <silent.h>

class ISigner;
class QThread;

namespace keys {
class KeysModule;
}

namespace keys::flow {

// Owns the durable fetch lock until the last device command has answered, even
// when its window closes. Storage work never runs on the GUI thread.
class LedgerNonces final : public QObject {
    Q_OBJECT

public:
    LedgerNonces(KeysModule *module, ISigner *device, QString instance_id, QString fingerprint,
                 QString descriptor, unsigned int quantity, QObject *owner);
    void start();

signals:
    void finished(QString error);
    void cacheChanged();

public slots:
    void onWorkFinished();
    void onDeviceSigned(ReqId req_id, const QString &psbt_base64);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message);
    void onInstanceRemoved(const QString &id);
    void onDeviceDestroyed();
    void onCancel();

protected:
    void finish(const QString &error);

private:
    struct Work {
        std::optional<rust::Box<::LedgerNonceFetch>> fetch;
        PsbtPayloadResult payload;
    };
    QPointer<ISigner> m_device;
    QString m_instance_id;
    QString m_fingerprint;
    QString m_descriptor;
    unsigned int m_quantity;
    std::shared_ptr<Work> m_work;
    QThread *m_thread = nullptr;
    ReqId m_req = 0;
    bool m_cancelled = false;
    bool m_finished = false;
};

} // namespace keys::flow
