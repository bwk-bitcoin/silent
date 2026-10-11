#pragma once

#include <optional>

#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>

#include <cstdint>

#include "FlowCopy.h"
#include "catalog/containers/Modal.h"

#include <interfaces/types.h>
#include <silent.h>

class ISigner;

namespace catalog {
class Button;
class Card;
class ChunkedAddress;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
class SelectRow;
} // namespace catalog

namespace qontrol {
class Column;
}

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::flow {

class CameraStep;
class QrDisplayStep;

// Where a key stands in the flow; it picks the row's pill, line and whether it can be
// picked.
enum class SignerStatus : std::uint8_t {
    First,
    Ready,
    Signed,
    Waiting,
    Disconnected,
    NoHot,
    NoRoute,
    Unregistered
};

// One key the request needs.
struct FlowRow {
    QString fingerprint;
    QString alias;
    // Unknown when the key has no route to show; a disconnected key keeps its USB one.
    KeyRouteKind route = KeyRouteKind::Unknown;
    // The live device's host instance id, for a Usb route.
    QString device_id;
    // The device as people know it ("Ledger"), empty when unknown.
    QString device;
    SignerStatus status = SignerStatus::NoRoute;

    [[nodiscard]] auto usable() const -> bool;
};

// A connected device that is not one of the request's keys.
struct OtherDevice {
    QString name;
    QString fingerprint;
};

// One modal per request: the signer picker, then the picked key's route step (USB,
// SD card, QR code and camera, hot key password), then for an address its result.
// Steps replace each other in the same window; closing it at any step cancels the
// request quietly. Answers exactly once, through one of the signals below.
class SigningFlow final : public catalog::Modal {
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

    enum class UsbError : std::uint8_t { Rejected, Disconnected, Failed };
    enum class VerifyResult : std::uint8_t { Matched, Mismatch, Error };

    SigningFlow(KeysModule *module, Request request);
    ~SigningFlow() override;
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
    void onRowClicked();
    void onUsbBack();
    void onUsbRetry();
    void onDeviceSigned(ReqId req_id, const QString &psbt_base64);
    void onDeviceAddressDisplayed(ReqId req_id, const QString &address);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message);
    void onInstanceRemoved(const QString &id);
    void onPasswordEdited();
    void onPasswordProceed();
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
    void initPicker();
    void initUsb();
    void initSd();
    void initPassword();
    void initBusy();
    void initQr();
    void initVerifyResult();
    void doConnect();
    void connectUsb();
    void connectSd();
    void connectPassword();
    void connectQr();
    void connectVerifyResult();
    void view();
    [[nodiscard]] auto viewPicker() -> QWidget *;
    [[nodiscard]] auto viewUsb() -> QWidget *;
    [[nodiscard]] auto viewSd() -> QWidget *;
    [[nodiscard]] auto viewPassword() -> QWidget *;
    [[nodiscard]] auto viewVerifyResult() -> QWidget *;
    [[nodiscard]] auto signRows() const -> std::optional<QList<FlowRow>>;
    [[nodiscard]] auto signRow(const SigningCandidate &candidate) const -> FlowRow;
    [[nodiscard]] auto verifyRows() const -> QList<FlowRow>;
    [[nodiscard]] auto fetchRow() const -> FlowRow;
    [[nodiscard]] auto otherDevices() const -> QList<OtherDevice>;
    // The name of a device remembered for `fingerprint`, for a key whose device is away.
    [[nodiscard]] auto rememberedDevice(const QString &fingerprint) const -> QString;
    [[nodiscard]] auto currentDeviceLabel() const -> QString;
    [[nodiscard]] auto hasPicker() const -> bool;
    [[nodiscard]] auto buildRow(const FlowRow &row, int index) const -> catalog::SelectRow *;
    void showPicker();
    void showUsb();
    void showUsbError(UsbError error);
    void showPassword();
    void showBusy();
    void showSd();
    void showQrDisplay();
    void showCamera();
    void showVerifyResult(VerifyResult result);
    void refreshSd();
    void startRoute();
    void startUsb();
    void startHot();
    void startSd();
    void startQr();
    void stopRoute();
    void releaseDevice();
    void rowSigned(const QString &psbt_v2_hex);
    void deliverDescriptor(const QString &descriptor);
    void fail(const QString &message);

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
    std::optional<VerifyResult> m_verify_result;
    bool m_answered = false;

    QPointer<ISigner> m_device;
    QString m_device_instance;
    ReqId m_device_req = 0;
    ReqId m_hot_req = 0;
    ReqId m_sd_req = 0;
    QString m_sd_dir;

    // Picker: one row per key the request needs, an empty state when there is none,
    // then the connected devices that cannot help.
    QWidget *m_picker_step = nullptr;
    qontrol::Column *m_picker_column = nullptr;

    // USB: a spinner while the device waits for the user, then why it failed, with
    // Back to the other signers and Try Again.
    QWidget *m_usb_step = nullptr;
    catalog::ModalStatus *m_usb_status = nullptr;
    catalog::Card *m_usb_address_card = nullptr;
    catalog::ChunkedAddress *m_usb_address = nullptr;
    catalog::ModalActions *m_usb_actions = nullptr;
    catalog::Button *m_usb_retry = nullptr;

    // SD card, in two rows: save the unsigned transaction, then load the signed one
    // back. A failed save or load is reported under the rows, with a retry.
    enum class SdPhase : std::uint8_t { Save, Load };
    enum class SdFailure : std::uint8_t { None, Write, Read };
    SdPhase m_sd_phase = SdPhase::Save;
    SdFailure m_sd_failure = SdFailure::None;
    QWidget *m_sd_step = nullptr;
    catalog::SelectRow *m_sd_save_row = nullptr;
    catalog::SelectRow *m_sd_load_row = nullptr;
    catalog::Notice *m_sd_write_error = nullptr;
    catalog::Notice *m_sd_read_error = nullptr;
    catalog::ModalActions *m_sd_actions = nullptr;
    catalog::Button *m_sd_save = nullptr;
    catalog::Button *m_sd_save_retry = nullptr;
    catalog::Button *m_sd_load = nullptr;
    catalog::Button *m_sd_load_retry = nullptr;

    // Hot key password: the value is handed over once and cleared; a QString cannot be
    // zeroized, so this is best effort.
    QWidget *m_password_step = nullptr;
    catalog::Label *m_password_prompt = nullptr;
    catalog::Input *m_password_input = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    catalog::Button *m_password_proceed = nullptr;

    catalog::ModalStatus *m_busy_step = nullptr;

    QrDisplayStep *m_qr = nullptr;
    CameraStep *m_camera = nullptr;

    // Verify result: what the device said about the address, with the address itself,
    // and Try Again unless it matched.
    QWidget *m_result_step = nullptr;
    catalog::ModalStatus *m_result_status = nullptr;
    catalog::ChunkedAddress *m_result_address = nullptr;
    catalog::ModalActions *m_result_actions = nullptr;
    catalog::Button *m_result_retry = nullptr;
};

} // namespace keys::flow
