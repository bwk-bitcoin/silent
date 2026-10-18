#pragma once

#include <optional>

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>

#include <interfaces/types.h>

class ISigner;

namespace keys::flow {

// Why a USB device did not sign or show what it was asked.
enum class UsbError : std::uint8_t { Rejected, Disconnected, Failed };

// bwk-hwi folds its error kinds into text at the device service boundary, so the kind
// is read back from its own messages (bwk hwi Error's Display).
auto usbErrorFor(const QString &message) -> UsbError;

// One signature of a PSBT on a connected USB device, with no window of its own: the
// PSBT goes out in the form the device takes and its answer is merged back into the
// original. Answers once, with signedPsbt() or failed(), then deletes itself.
class DeviceSign final : public QObject {
    Q_OBJECT

public:
    DeviceSign(ISigner *device, QString instance_id, QString psbt_v2_hex,
               QObject *parent = nullptr);
    ~DeviceSign() override;
    // Asks the device to sign for the wallet `descriptor`.
    void start(const QString &descriptor);

signals:
    void signedPsbt(QString psbt_v2_hex);
    // `error` says what happened on the device; `message` is its own text.
    void failed(UsbError error, QString message, plugin::SignError kind);

public slots:
    void onDeviceSigned(ReqId req_id, const QString &psbt_base64);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    // An unplugged device never answers: its request fails here.
    void onInstanceRemoved(const QString &id);

protected:
    // Stops listening to the device and deletes this once the answer is out.
    void finish();
    void fail(UsbError error, const QString &message,
              plugin::SignError kind = plugin::SignError::Other);

private:
    QPointer<ISigner> m_device;
    QString m_instance_id;
    QString m_psbt_v2_hex;
    ReqId m_req = 0;
};

} // namespace keys::flow
