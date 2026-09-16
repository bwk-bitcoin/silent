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
class QrDisplay;
} // namespace keys::modal

namespace keys {

// Owns the whole QR route: every sign request funnels through the single m_flow
// this class holds, so exactly one can be in flight at a time and every ending,
// whatever triggers it, passes through finishFlow().
class QrRoute final : public QObject {
    Q_OBJECT

public:
    explicit QrRoute(KeysModule *module);
    ~QrRoute() override = default;

    // Starts a sign flow. Answers at once with keys-qr-busy through `signer` if a
    // flow is already in progress.
    void startSign(ReqId req_id, RouteSigner *signer, QString psbt_v2_hex, QString descriptor);

signals:
    // Emitted from the background thread buildSignRequest() spawns, with the
    // freshly built (or rebuilt) QR request.
    void qrRequestReady(QrRequest *request);

public slots:
    void onQrRequestReady(QrRequest *request);
    void onDensityChanged(int density);
    void onDisplayNext();
    void onDisplayRejected();

protected:
    // The two possible endings that hand an answer back to the caller.
    enum class Outcome : std::uint8_t { Signed, Error };

    // Reserves a QR request id on the GUI thread, then builds (or rebuilds, at a
    // new density) the sign request's frames on a background thread, emitting
    // qrRequestReady() with the raw pointer once built.
    void buildSignRequest();
    // Shows `request` in m_display, opening it on the first call and swapping the
    // request into the already-open modal on every rebuild after that.
    void showRequest(rust::Box<QrRequest> request);
    // The only place that ends a flow: answers m_flow's caller exactly once (a
    // second call is a no-op, since m_flow is already reset by the first), then
    // removes the pending request registration, closes the display if still open,
    // and resets m_flow.
    void finishFlow(Outcome outcome, const QString &text);
    // Shared tail of finishFlow().
    void closeOutstanding(ReqId req_id);

private:
    // One QR sign flow in progress: the caller's request id, the RouteSigner
    // waiting on the answer, the reserved-but-uncommitted request id (0 once
    // committed), and the request's own data, kept to rebuild at a new density.
    struct QrFlow {
        ReqId req_id = 0;
        QPointer<RouteSigner> signer;
        std::uint64_t pending_id = 0;
        QString psbt_hex;
        QString descriptor;
    };

    KeysModule *m_module = nullptr;
    std::optional<QrFlow> m_flow;
    QPointer<modal::QrDisplay> m_display;
    int m_density;
};

} // namespace keys
