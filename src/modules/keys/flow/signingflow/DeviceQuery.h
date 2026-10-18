#pragma once

#include <optional>

#include <QObject>
#include <QPointer>
#include <QString>

#include <interfaces/types.h>

#include "DeviceSign.h"

class ISigner;

namespace keys::flow {

// One xpub fetched, or one address shown, on a connected USB device, with no window of
// its own. Answers once, with xpubFetched(), addressShown() or failed(), then deletes
// itself.
class DeviceQuery final : public QObject {
    Q_OBJECT

public:
    DeviceQuery(ISigner *device, QString instance_id, QObject *parent = nullptr);
    ~DeviceQuery() override;
    // Asks the device for its xpub at `path`.
    void fetchXpub(const QString &path);
    // Asks the device to show `address`, at `path` of the wallet `descriptor`.
    void showAddress(const QString &descriptor, const QString &path, const QString &address);

signals:
    void xpubFetched(QString xpub);
    // `matched` false: the device refused the address, its answer that it does not match.
    void addressShown(bool matched);
    // `error` says what happened on the device; `message` is its own text.
    void failed(UsbError error, QString message);

public slots:
    void onDeviceXpub(ReqId req_id, const QString &xpub);
    void onDeviceAddressDisplayed(ReqId req_id, const QString &address);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    // An unplugged device never answers: its request fails here.
    void onInstanceRemoved(const QString &id);

protected:
    // Stops listening to the device and deletes this once the answer is out.
    void finish();
    void fail(UsbError error, const QString &message);

private:
    QPointer<ISigner> m_device;
    QString m_instance_id;
    ReqId m_req = 0;
    bool m_showing_address = false;
};

} // namespace keys::flow
