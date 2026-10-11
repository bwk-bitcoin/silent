#include "qr/QrCameraEngine.h"

#include <QThread>

#include <algorithm>
#include <cmath>
#include <utility>

#include <common.h>

namespace qr {

namespace {

// The contrast scales each channel around mid grey (the design's CSS
// contrast(0.55 + level * 0.28)).
constexpr double CONTRAST_BASE = 0.55;
constexpr double CONTRAST_STEP = 0.28;
constexpr double MID_GREY = 128.0;
constexpr int CHANNEL_LEVELS = 256;
constexpr int MAX_CHANNEL = 255;
constexpr int RGB_CHANNELS = 3;

} // namespace

QrScanWorker::QrScanWorker(std::unique_ptr<QrScanner> scanner) : m_scanner(std::move(scanner)) {
}

void QrScanWorker::process(const QImage &image) {
    emit scanned(m_scanner->scan(image));
}

void QrScanWorker::reset() {
    m_scanner->clear();
}

QrCameraEngine::QrCameraEngine(std::unique_ptr<QrScanner> scanner, const QrCameraSettings &settings,
                               QObject *parent)
    : QObject(parent),
      m_settings(settings) {
    rebuildContrastTable();
    m_thread = new QThread(this);
    m_worker = new QrScanWorker(std::move(scanner));
    m_worker->moveToThread(m_thread);
    connect(this, &QrCameraEngine::scanRequested, m_worker, &QrScanWorker::process,
            qontrol::UNIQUE);
    connect(this, &QrCameraEngine::resetRequested, m_worker, &QrScanWorker::reset, qontrol::UNIQUE);
    connect(m_worker, &QrScanWorker::scanned, this, &QrCameraEngine::onScanned, qontrol::UNIQUE);
    m_thread->start();
}

QrCameraEngine::~QrCameraEngine() {
    m_thread->quit();
    m_thread->wait();
    // The thread is over: the worker is deleted here rather than on its own thread.
    delete m_worker;
}

void QrCameraEngine::setSettings(const QrCameraSettings &settings) {
    m_settings = settings;
    rebuildContrastTable();
}

auto QrCameraEngine::settings() const -> QrCameraSettings {
    return m_settings;
}

void QrCameraEngine::reset() {
    emit resetRequested();
}

void QrCameraEngine::frame(const CameraFrame &frame) {
    QImage image = applyContrast(frame.image);
    emit broadcast(CameraFrame{.image = image, .seq = frame.seq});
    if (m_busy) {
        return;
    }
    m_busy = true;
    emit scanRequested(image);
}

void QrCameraEngine::onScanned(const QrScan &scan) {
    m_busy = false;
    emit progress(scan.seen, scan.total);
    for (const auto &payload : scan.payloads) {
        emit response(QrResponse{.payload = payload});
    }
}

void QrCameraEngine::rebuildContrastTable() {
    double factor = CONTRAST_BASE + m_settings.contrast * CONTRAST_STEP;
    m_contrast_table.resize(CHANNEL_LEVELS);
    for (int level = 0; level < CHANNEL_LEVELS; ++level) {
        auto value = static_cast<int>(std::lround((level - MID_GREY) * factor + MID_GREY));
        m_contrast_table[level] = static_cast<std::uint8_t>(std::clamp(value, 0, MAX_CHANNEL));
    }
}

auto QrCameraEngine::applyContrast(const QImage &image) const -> QImage {
    QImage out = image.convertToFormat(QImage::Format_RGB888);
    for (int row = 0; row < out.height(); ++row) {
        std::uint8_t *line = out.scanLine(row);
        for (int byte = 0; byte < out.width() * RGB_CHANNELS; ++byte) {
            line[byte] = m_contrast_table.at(line[byte]);
        }
    }
    return out;
}

} // namespace qr
