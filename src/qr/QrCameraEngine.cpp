#include "qr/QrCameraEngine.h"

#include <QThread>

#include <algorithm>
#include <cmath>

#include <common.h>

namespace qr {

namespace {

// The contrast scales each channel around mid grey, as the design's CSS contrast().
constexpr double PERCENT = 100.0;
constexpr double MID_GREY = 128.0;
constexpr int CHANNEL_LEVELS = 256;
constexpr int MAX_CHANNEL = 255;
constexpr int RGB_CHANNELS = 3;

} // namespace

QrScanWorker::QrScanWorker(QrCameraEngine *engine) : m_engine(engine) {
}

void QrScanWorker::process(const QImage &image) {
    emit scanned(m_engine->scan(image));
}

void QrScanWorker::reset() {
    m_engine->clear();
}

QrCameraEngine::QrCameraEngine(QObject *parent) : QObject(parent) {
    rebuildContrastTable();
    m_thread = new QThread(this);
    m_worker = new QrScanWorker(this);
    m_worker->moveToThread(m_thread);
    connect(this, &QrCameraEngine::scanRequested, m_worker, &QrScanWorker::process,
            qontrol::UNIQUE);
    connect(this, &QrCameraEngine::resetRequested, m_worker, &QrScanWorker::reset, qontrol::UNIQUE);
    connect(m_worker, &QrScanWorker::scanned, this, &QrCameraEngine::onScanned, qontrol::UNIQUE);
    m_thread->start();
}

QrCameraEngine::~QrCameraEngine() {
    stopWorker();
}

void QrCameraEngine::stopWorker() {
    if (m_worker == nullptr) {
        return;
    }
    m_thread->quit();
    m_thread->wait();
    // The thread is over: the worker is deleted here rather than on its own thread.
    delete m_worker;
    m_worker = nullptr;
}

void QrCameraEngine::setContrast(QrCameraContrast contrast) {
    m_contrast = contrast;
    rebuildContrastTable();
}

auto QrCameraEngine::contrast() const -> QrCameraContrast {
    return m_contrast;
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
    double factor = static_cast<int>(m_contrast) / PERCENT;
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
