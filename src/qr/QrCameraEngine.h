#pragma once

#include "qr/QrTypes.h"

#include <QImage>
#include <QList>
#include <QObject>

#include <cstdint>

class QThread;

namespace qr {

class QrCameraEngine;

// Runs its camera engine's scan on the engine's worker thread.
class QrScanWorker : public QObject {
    Q_OBJECT

public:
    explicit QrScanWorker(QrCameraEngine *engine);

signals:
    void scanned(const qr::QrScan &scan);

public slots:
    void process(const QImage &image);
    void reset();

private:
    QrCameraEngine *m_engine = nullptr;
};

// Reads payloads back off camera frames, protocol agnostic: applies the contrast to
// each received frame, shows it (broadcast), and hands it to the format's scan() on a
// single worker thread, dropping the frames that come while it is busy. Each transport
// inherits it and implements scan() and clear().
//
// scan() and clear() run on the worker thread, so every subclass destructor calls
// stopWorker() first: once the subclass is gone the worker would call a pure virtual.
class QrCameraEngine : public QObject {
    Q_OBJECT
    friend class QrScanWorker;

public:
    // The contrast starts at its Medium.
    explicit QrCameraEngine(QObject *parent = nullptr);
    ~QrCameraEngine() override;
    // Applied to each received frame before it is scanned.
    void setContrast(QrCameraContrast contrast);
    [[nodiscard]] auto contrast() const -> QrCameraContrast;
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
    // Joins the payloads read off successive camera frames. On the worker thread, one
    // call at a time.
    virtual auto scan(const QImage &image) -> QrScan = 0;
    // Drops the parts seen so far. On the worker thread.
    virtual void clear() = 0;
    void stopWorker();
    // Rebuilt only when the contrast changes, so the per-frame path never allocates it.
    void rebuildContrastTable();
    [[nodiscard]] auto applyContrast(const QImage &image) const -> QImage;

private:
    QrCameraContrast m_contrast = QrCameraContrast::Medium;
    QThread *m_thread = nullptr;
    QrScanWorker *m_worker = nullptr;
    // Per channel value after the contrast.
    QList<std::uint8_t> m_contrast_table;
    bool m_busy = false;
};

} // namespace qr
