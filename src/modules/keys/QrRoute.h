#pragma once

#include <cstdint>
#include <optional>

#include <QObject>
#include <QPointer>
#include <QString>

#include <interfaces/types.h>
#include <silent.h>

namespace keys {
class KeysModule;
class RouteSigner;
} // namespace keys

namespace keys::modal {
class CameraScan;
class QrDisplay;
} // namespace keys::modal

namespace keys {

// Owns the whole QR route: sign and SP descriptor requests all funnel
// through the single m_flow this class holds, so exactly one of them can be in
// flight at a time and every ending, whatever triggers it, passes through
// finishFlow().
class QrRoute final : public QObject {
    Q_OBJECT

public:
    explicit QrRoute(KeysModule *module);
    ~QrRoute() override = default;

    // Starts a sign flow. Answers at once with keys-qr-busy through `signer` if a
    // flow is already in progress.
    void startSign(ReqId req_id, RouteSigner *signer, QString psbt_v2_hex, QString descriptor);
    // Starts an sp() descriptor fetch. Answers through the module's bus (there is
    // no RouteSigner for this flow), same busy behaviour as startSign().
    void startSpDescriptor(ReqId req_id);
    // Camera-sourced events for the flow currently in progress: a decoded/validated
    // response, a scan error, or a scan progress update.
    void onKeyEvent(const KeyEvent &event);

signals:
    // Emitted with the freshly built (or rebuilt) QR request: synchronously for a
    // spDescriptor request, and from the background thread buildSignRequest() spawns
    // for a sign request.
    void qrRequestReady(QrRequest *request);

public slots:
    void onQrRequestReady(QrRequest *request);
    void onDensityChanged(int density);
    void onDisplayNext();
    void onDisplayRejected();
    void onCameraScanPrevious();
    void onCameraScanCancelled();

protected:
    // The three possible endings that hand an answer back to the caller.
    enum class Outcome : std::uint8_t { Signed, Descriptor, Error };

    // Reserves a QR request id on the GUI thread, then builds (or rebuilds, at a
    // new density) the sign request's frames on a background thread, emitting
    // qrRequestReady() with the raw pointer once built.
    void buildSignRequest();
    // Same as buildSignRequest(), synchronous, for the current spDescriptor flow.
    void buildSpDescriptorRequest();
    // Shows `request` in m_display, opening it on the first call and swapping the
    // request into the already-open modal on every rebuild after that.
    void showRequest(rust::Box<QrRequest> request);
    // The only place that ends a flow: answers m_flow's caller exactly once (a
    // second call is a no-op, since m_flow is already reset by the first), then
    // removes the pending request registration, closes the camera scan and the
    // display if still open, and resets m_flow.
    void finishFlow(Outcome outcome, const QString &text);
    // Shared tail of finishFlow().
    void closeOutstanding(ReqId req_id);

private:
    // One QR flow in progress: which kind it is, the caller's request id, the
    // RouteSigner waiting on the answer (null for SpDescriptor, answered through
    // the module's bus instead), the sign flow's reserved-but-uncommitted request
    // id (0 once committed or for a non-sign flow), and the request's own data,
    // kept to rebuild at a new density.
    struct QrFlow {
        enum class Kind : std::uint8_t { Sign, SpDescriptor };

        Kind kind{};
        ReqId req_id = 0;
        QPointer<RouteSigner> signer;
        std::uint64_t pending_id = 0;
        QString psbt_hex;
        QString descriptor;
    };

    KeysModule *m_module = nullptr;
    std::optional<QrFlow> m_flow;
    QPointer<modal::QrDisplay> m_display;
    QPointer<modal::CameraScan> m_camera_scan;
    int m_density;
};

} // namespace keys
