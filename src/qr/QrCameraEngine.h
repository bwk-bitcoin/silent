#pragma once

#include "qr/QrTypes.h"

#include <QImage>
#include <QList>
#include <QObject>

#include <cstdint>
#include <memory>

class QThread;

namespace qr {

// A transport's scan: joins the payloads it reads off successive camera frames. Runs on
// its engine's worker thread, one call at a time.
class QrScanner {
public:
    virtual ~QrScanner() = default;
    virtual auto scan(const QImage &image) -> QrScan = 0;
    // Drops the parts seen so far.
    virtual void clear() = 0;
};

// Owns a QrScanner on the camera engine's worker thread.
class QrScanWorker : public QObject {
    Q_OBJECT

public:
    explicit QrScanWorker(std::unique_ptr<QrScanner> scanner);

signals:
    void scanned(const qr::QrScan &scan);

public slots:
    void process(const QImage &image);
    void reset();

private:
    std::unique_ptr<QrScanner> m_scanner;
};

// Reads payloads back off camera frames, protocol agnostic: applies the contrast to
// each received frame, shows it (broadcast), and hands it to the format's QrScanner on
// a single worker thread, dropping the frames that come while it is busy. Each
// transport inherits it with its scanner.
class QrCameraEngine : public QObject {
    Q_OBJECT

public:
    QrCameraEngine(std::unique_ptr<QrScanner> scanner, const QrCameraSettings &settings,
                   QObject *parent = nullptr);
    ~QrCameraEngine() override;
    void setSettings(const QrCameraSettings &settings);
    [[nodiscard]] auto settings() const -> QrCameraSettings;
    // Drops the parts seen so far, for a scan from scratch.
    void reset();

signals:
    // Each received frame with the contrast applied: what the scan reads.
    void broadcast(const qr::CameraFrame &frame);
    void response(const qr::QrResponse &response);
    // Parts seen of the total of the payload being joined, 0 of 0 while none is.
    void progress(int seen, int total);
    // To the worker.
    void scanRequested(const QImage &image);
    void resetRequested();

public slots:
    void frame(const qr::CameraFrame &frame);
    void onScanned(const qr::QrScan &scan);

protected:
    // Rebuilt only when the contrast changes, so the per-frame path never allocates it.
    void rebuildContrastTable();
    [[nodiscard]] auto applyContrast(const QImage &image) const -> QImage;

private:
    QrCameraSettings m_settings;
    QThread *m_thread = nullptr;
    QrScanWorker *m_worker = nullptr;
    // Per channel value after the contrast.
    QList<std::uint8_t> m_contrast_table;
    bool m_busy = false;
};

} // namespace qr
