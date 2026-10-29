#include "modules/keys/BwkQrEngine.h"
#include "modules/keys/KeysModule.h"
#include "qr/QrEngine.h"
#include "theme/Theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QThread>

#include <common.h>
#include <functional>
#include <memory>
#include <utility>

namespace smoke {
namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("QR cache exchange smoke: %s", message);
    }
}

// Counts qr::QrEngine::request emissions, keeping the wire bytes of the first one.
// Named slot, polled: the answer arrives from a worker thread.
class RequestSpy final : public QObject {
    Q_OBJECT

public:
    explicit RequestSpy(QObject *parent = nullptr) : QObject(parent) {
    }

    ~RequestSpy() override = default;

    [[nodiscard]] auto count() const -> int {
        return m_count;
    }

    [[nodiscard]] auto payload() const -> QByteArray {
        return m_payload;
    }

public slots:

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void onRequest(const qr::QrRequest &request) {
        if (m_count == 0) {
            m_payload = request.payload;
        }
        ++m_count;
    }

private:
    int m_count = 0;
    QByteArray m_payload;
};

// Counts qr::QrEngine::error emissions, through a named slot.
class ErrorSpy final : public QObject {
    Q_OBJECT

public:
    explicit ErrorSpy(QObject *parent = nullptr) : QObject(parent) {
    }

    ~ErrorSpy() override = default;

    [[nodiscard]] auto count() const -> int {
        return m_count;
    }

public slots:

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void onError([[maybe_unused]] qr::QrError kind, [[maybe_unused]] const QString &message) {
        ++m_count;
    }

private:
    int m_count = 0;
};

// Spins the event loop until `done` holds or the budget runs out.
void waitUntil(const std::function<bool()> &done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 10000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

// A real musig policy and the matching key fingerprint, the same fixture the Rust
// exchange tests use. The smoke exercises the request itself; the refused answer
// stands in for a peer that never prepared.
const char *const POLICY =
    "tr(musig([73c5da0a/87h/1h/0h]tpubDCChhoz5Qdrkn7Z7KXawq6Ad6r3A4MUkCoVTqeWxfTkA6bHNJ3CHUEtALQ"
    "dkNeixNz4446PcAmw4WKcj3mV2vb29H7sg9EPzbyCU1y2merw/<0;1>/*,[b8688df1/87h/1h/0h]"
    "tpubDC44ZFebbFm5ZXnG5Xt8k9PpdSZXUQ4HyuU8e8u5zfutYuu8EW4MLKS54EFTfGYgeqt7P2jEEd1iCF5kSPi7Xrj"
    "6EZcsStCcAjPy97DSiLb/<0;1>/*))";
const char *const FINGERPRINT = "73c5da0a";

} // namespace

void qrCacheExchangeSmoke() {
    Theme::get()->apply();
    auto module = std::make_unique<keys::KeysModule>();
    keys::BwkQrEngine engine(module.get());
    RequestSpy requests;
    ErrorSpy errors;
    QObject::connect(&engine, &qr::QrEngine::request, &requests, &RequestSpy::onRequest,
                     qontrol::UNIQUE);
    QObject::connect(&engine, &qr::QrEngine::error, &errors, &ErrorSpy::onError, qontrol::UNIQUE);

    // The worker persists the record and queues the material; the engine turns it
    // into the wire request only when the queued result is handled.
    engine.prepareMusig(QString::fromLatin1(FINGERPRINT), QString::fromLatin1(POLICY));
    waitUntil([&requests] { return requests.count() > 0; });
    require(requests.count() == 1, "the preparation ended in exactly one wire request");
    require(!requests.payload().isEmpty(), "the wire request carries bytes");
    require(errors.count() == 0, "a clean preparation reports no error");

    // A scanned answer that the cache refuses still delivers its outcome, and the
    // engine takes no second request for it.
    QByteArray garbage("not a protocol message");
    engine.response({.payload = garbage});
    waitUntil([&errors] { return errors.count() > 0; });
    require(errors.count() == 1, "the refused answer delivers an error outcome");
    require(requests.count() == 1, "a refused answer never issues another request");
}

} // namespace smoke

#include "qr_cache_smoke.moc"
