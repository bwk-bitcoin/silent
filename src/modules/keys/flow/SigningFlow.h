#pragma once

#include <optional>

#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>

#include "FlowCopy.h"
#include "PickerStep.h"
#include "VerifyResultStep.h"
#include "catalog/containers/StepModal.h"

#include <interfaces/types.h>
#include <silent.h>

class ISigner;

namespace catalog {
class ModalStatus;
}

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::flow {

class CameraStep;
class PasswordStep;
class QrDisplayStep;
class SdStep;
class UsbStep;

// One modal per request: the signer picker, then the picked key's route step (USB,
// SD card, QR code and camera, hot key password), then for an address its result.
// Steps replace each other in the same window; closing it at any step cancels the
// request quietly. Answers exactly once, through one of the signals below.
class SigningFlow final : public catalog::StepModal {
    Q_OBJECT

public:
    struct Request {
        Context context = Context::Sign;
        ReqId req_id = 0;
        // Sign.
        QString psbt_v2_hex;
        // Sign and verify: the wallet the keys sign for.
        QString descriptor;
        // Sign: names the SD card files.
        QString wallet_name;
        // Verify.
        QString path;
        QString address;
        // Fetch: the one key asked, so there is no picker.
        QString fingerprint;
    };

    SigningFlow(KeysModule *module, Request request);
    ~SigningFlow() override = default;
    // Shows the first step. Call once the answer signals are connected: a route that
    // fails at once answers before the window opens (see answered()).
    void begin();
    [[nodiscard]] auto answered() const -> bool;

signals:
    void psbtSigned(ReqId req_id, QString psbt_v2_hex);
    void addressVerified(ReqId req_id, bool matched);
    void descriptorFetched(ReqId req_id, QString descriptor, QString fingerprint);
    // A failure the flow cannot recover from; the window closes.
    void failed(ReqId req_id, QString message);
    // Closed before an answer, or after showing why there is none.
    void cancelled(ReqId req_id);

public slots:
    void done(int result) override;
    void onRowPicked(int index);
    void onUsbBack();
    void onUsbRetry();
    void onDeviceSigned(ReqId req_id, const QString &psbt_base64);
    void onDeviceAddressDisplayed(ReqId req_id, const QString &address);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message);
    void onInstanceRemoved(const QString &id);
    void onPasswordEntered(QString password);
    void onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex, const QString &error,
                     bool wrong_password);
    void onHotDescriptorFetched(ReqId req_id, bool is_ok, const QString &descriptor,
                                const QString &error, bool wrong_password);
    void onSdSaveClicked();
    void onSdLoadClicked();
    void onSdSaved(ReqId req_id, bool is_ok, const QString &error);
    void onSdLoaded(ReqId req_id, bool is_ok, const QString &psbt_hex, const QString &error);
    void onQrNext();
    void onCameraBack();
    void onQrSigned(const QString &psbt_v2_hex);
    void onQrVerified(bool matched);
    void onQrDescriptor(const QString &descriptor);
    void onQrScanFailed(QrErrorKind kind, const QString &message);
    void onQrFailed(const QString &message);
    void onVerifyRetry();

protected:
    void init();
    void doConnect();
    void view();
    [[nodiscard]] auto signRows() const -> std::optional<QList<FlowRow>>;
    [[nodiscard]] auto signRow(const SigningCandidate &candidate) const -> FlowRow;
    [[nodiscard]] auto verifyRows() const -> QList<FlowRow>;
    [[nodiscard]] auto fetchRow() const -> FlowRow;
    [[nodiscard]] auto otherDevices() const -> QList<OtherDevice>;
    // The name of a device remembered for `fingerprint`, for a key whose device is away.
    [[nodiscard]] auto rememberedDevice(const QString &fingerprint) const -> QString;
    [[nodiscard]] auto currentDeviceLabel() const -> QString;
    [[nodiscard]] auto hasPicker() const -> bool;
    void showPicker();
    void startRoute();
    void startUsb();
    void startHot();
    void startSd();
    void startQr();
    void stopRoute();
    void releaseDevice();
    void rowSigned(const QString &psbt_v2_hex);
    void showVerifyResult(VerifyResultStep::Result result);
    void deliverDescriptor(const QString &descriptor);
    void fail(const QString &message);
    // Resizes the window to the current step after its content changed.
    void refit();

private:
    KeysModule *m_module = nullptr;
    Request m_request;
    FlowCopy m_copy;
    // The PSBT as signed so far.
    QString m_psbt;
    QList<FlowRow> m_rows;
    // Keys that signed in this flow.
    QSet<QString> m_signed;
    std::optional<FlowRow> m_current;
    std::optional<VerifyResultStep::Result> m_verify_result;
    bool m_answered = false;

    QPointer<ISigner> m_device;
    QString m_device_instance;
    ReqId m_device_req = 0;
    ReqId m_hot_req = 0;
    ReqId m_sd_req = 0;
    QString m_sd_dir;

    PickerStep *m_picker = nullptr;
    UsbStep *m_usb = nullptr;
    SdStep *m_sd = nullptr;
    PasswordStep *m_password = nullptr;
    catalog::ModalStatus *m_busy = nullptr;
    QrDisplayStep *m_qr = nullptr;
    CameraStep *m_camera = nullptr;
    VerifyResultStep *m_result = nullptr;
    int m_picker_page = 0;
    int m_usb_page = 0;
    int m_sd_page = 0;
    int m_password_page = 0;
    int m_busy_page = 0;
    int m_qr_page = 0;
    int m_camera_page = 0;
    int m_result_page = 0;
};

} // namespace keys::flow
