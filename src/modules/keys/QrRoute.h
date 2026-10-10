#pragma once

#include <cstdint>
#include <optional>

#include <QObject>
#include <QPointer>
#include <QString>

#include <silent.h>

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::flow {
class QrDisplayStep;
} // namespace keys::flow

namespace keys {

// Runs one QR exchange at a time (sign, verify an address, fetch the sp() descriptor,
// fetch an xpub):
// builds the request into the display step it is given, rebuilds it when the density
// changes, and turns the camera's decoded answer into the matching signal.
class QrRoute final : public QObject {
    Q_OBJECT

public:
    explicit QrRoute(KeysModule *module);
    ~QrRoute() override = default;

    // Each start ends the exchange in progress, if any.
    void startSign(flow::QrDisplayStep *display, QString psbt_v2_hex, QString descriptor);
    void startVerify(flow::QrDisplayStep *display, QString descriptor, QString path,
                     QString address);
    void startSpDescriptor(flow::QrDisplayStep *display);
    // Asks for the xpub at `path`, written m/...
    void startXpub(flow::QrDisplayStep *display, QString path);
    // Ends the exchange: a request still building or a late answer is dropped.
    void stop();
    void onKeyEvent(const KeyEvent &event);

signals:
    // Emitted from the worker thread that built a sign request.
    void requestBuilt(QrRequest *request);
    void psbtSigned(QString psbt_v2_hex);
    void addressVerified(bool matched);
    void descriptorDecoded(QString descriptor);
    // `path` is written m/...
    void xpubDecoded(QString fingerprint, QString path, QString xpub);
    // The camera refused a code, or failed; the scan goes on.
    void scanFailed(QrErrorKind kind, QString message);
    // The request could not be built; the exchange is over.
    void failed(QString message);

public slots:
    void onRequestBuilt(QrRequest *request);
    void onDensityChanged(int density);

protected:
    enum class Kind : std::uint8_t { Sign, Verify, SpDescriptor, Xpub };

    // The request's own data, kept to rebuild it at another density. `pending_id` is
    // the sign request id reserved and not yet committed (0 otherwise).
    struct Exchange {
        Kind kind{};
        std::uint64_t pending_id = 0;
        QString psbt_hex;
        QString descriptor;
        QString path;
        QString address;
    };

    void start(flow::QrDisplayStep *display, Exchange exchange);
    void build();
    // Reserves the sign request id on the GUI thread, then builds the frames on a
    // worker thread, answered through requestBuilt().
    void buildSign();
    void buildVerify();
    void buildSpDescriptor();
    void buildXpub();
    void decodeXpub(const KeyEvent &event);
    void show(rust::Box<QrRequest> request);
    void fail(const QString &message);
    [[nodiscard]] auto partBytes() const -> std::uint32_t;

private:
    KeysModule *m_module = nullptr;
    std::optional<Exchange> m_exchange;
    QPointer<flow::QrDisplayStep> m_display;
};

} // namespace keys
