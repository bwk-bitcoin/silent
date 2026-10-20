#pragma once

#include <optional>

#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdint>

#include "FlowCopy.h"
#include "catalog/containers/Modal.h"
#include "qr/QrTypes.h"
#include "theme/Palette.h"

#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class SelectRow;
class ViewSlots;
} // namespace catalog

namespace qontrol {
class Column;
}

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::flow {

class SigningRouteFlow;

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

// One modal per request: the signer picker, then the route of the picked key embedded
// in its place (USB, SD card, QR code and camera, hot key password), which for an
// address ends on its result. Closing the window at any step cancels the request
// quietly. Answers exactly once, through one of the signals below.
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
        // Verify, and FetchXpub: the path of the xpub.
        QString path;
        // Verify.
        QString address;
        // Fetch and FetchXpub: the one key asked, so there is no picker. Sign: when set,
        // only this key signs, with no picker, and the flow answers once it has. Verify:
        // when set, only this key shows the address, with no picker.
        QString fingerprint;
    };

    enum class VerifyResult : std::uint8_t { Matched, Mismatch, Error };

    SigningFlow(KeysModule *module, Request request);
    // Shows the first step. Call once the answer signals are connected: a route that
    // fails at once answers before the window opens (see answered()).
    void begin();
    [[nodiscard]] auto answered() const -> bool;
    void done(int result) override;

signals:
    void psbtSigned(ReqId req_id, QString psbt_v2_hex);
    void addressVerified(ReqId req_id, bool matched);
    // `xpubs` are the key's sub-account xpubs the route handed with the descriptor.
    void descriptorFetched(ReqId req_id, QString descriptor, QString fingerprint,
                           QList<qr::QrXpub> xpubs);
    void xpubFetched(ReqId req_id, QString xpub, QString fingerprint, QString path);
    // A failure the flow cannot recover from; the window closes.
    void failed(ReqId req_id, QString message, plugin::SignError kind = plugin::SignError::Other);
    // Closed before an answer, or after showing why there is none.
    void cancelled(ReqId req_id);

public slots:
    void onNonceSnapshotReady();
    void onRowClicked();
    // A device came, went or changed state: the picker on display lists it as it is.
    void onDevicesChanged();
    void onFlowViewChanged(const QString &title, ModalWidth width);
    void onFlowBack();
    void onPsbtSigned(const QString &psbt_v2_hex);
    void onVerifyResultChanged(std::optional<SigningFlow::VerifyResult> result);
    void onDescriptorFetched(const QString &descriptor, const QList<qr::QrXpub> &xpubs);
    void onXpubFetched(const QString &xpub);
    void onFailed(const QString &message, plugin::SignError kind = plugin::SignError::Other);

protected:
    void init();
    void view();

    // Picker view
    void initPicker();
    [[nodiscard]] auto viewPicker() -> QWidget *;
    void showPicker();
    void hidePicker();
    [[nodiscard]] auto signRows() const -> std::optional<QList<FlowRow>>;
    [[nodiscard]] auto signRow(const SigningCandidate &candidate) const -> FlowRow;
    [[nodiscard]] auto verifyRows() const -> QList<FlowRow>;
    [[nodiscard]] auto fetchRow() const -> FlowRow;
    [[nodiscard]] auto otherDevices() const -> QList<OtherDevice>;
    // The name of a device remembered for `fingerprint`, for a key whose device is away.
    [[nodiscard]] auto rememberedDevice(const QString &fingerprint) const -> QString;
    [[nodiscard]] auto buildRow(const FlowRow &row, int index) const -> catalog::SelectRow *;

    // Shared
    // Embeds the route of the key picked.
    void startRoute();
    // Sign or verify with the one key the request names: its route, or a failure saying
    // why not.
    void startRequestedRow();
    void closeFlow();

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
    bool m_nonce_loaded = false;

    // Picker: one row per key the request needs, an empty state when there is none,
    // then the connected devices that cannot help.
    QWidget *m_picker_step = nullptr;
    qontrol::Column *m_picker_column = nullptr;

    // The route of the key picked, deleted once it goes back or the window closes.
    SigningRouteFlow *m_flow = nullptr;
};

// One route to a key, embedded in SigningFlow once the key is picked (at once for a
// fetch): its views, their button bars and the work behind them. Deleting it stops
// that work.
class SigningRouteFlow : public QWidget {
    Q_OBJECT

public:
    SigningRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row, QString psbt);
    // Starts the route, once SigningFlow listens to the signals.
    virtual void start() = 0;

signals:
    // The window title and width of the view now shown.
    void viewChanged(const QString &title, ModalWidth width);
    // Back left the route: SigningFlow shows its signer picker again.
    void back();
    // The key signed: the PSBT as signed so far.
    void psbtSigned(const QString &psbt_v2_hex);
    // What the device said about the address, none while it is asked again.
    void verifyResultChanged(std::optional<SigningFlow::VerifyResult> result);
    // `xpubs` are the sub-account xpubs handed along, none on a route that has none.
    void descriptorFetched(const QString &descriptor, const QList<qr::QrXpub> &xpubs);
    void xpubFetched(const QString &xpub);
    // A failure the route cannot recover from: SigningFlow closes. `kind` says why,
    // for the caller to retry or restart.
    void failed(const QString &message, plugin::SignError kind = plugin::SignError::Other);
    // Left with no picker to go back to: SigningFlow closes as cancelled.
    void cancelled();

protected:
    [[nodiscard]] auto viewSlots() const -> catalog::ViewSlots *;
    [[nodiscard]] auto module() const -> KeysModule *;
    [[nodiscard]] auto request() const -> const SigningFlow::Request &;
    [[nodiscard]] auto copy() const -> const FlowCopy &;
    [[nodiscard]] auto row() const -> const FlowRow &;
    // The PSBT as signed before this route.
    [[nodiscard]] auto psbt() const -> const QString &;
    [[nodiscard]] auto hasPicker() const -> bool;
    [[nodiscard]] auto deviceLabel() const -> QString;

private:
    catalog::ViewSlots *m_slots = nullptr;
    KeysModule *m_module = nullptr;
    SigningFlow::Request m_request;
    FlowCopy m_copy;
    FlowRow m_row;
    QString m_psbt;
};

} // namespace keys::flow
