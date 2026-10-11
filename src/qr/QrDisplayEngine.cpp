#include "qr/QrDisplayEngine.h"

#include "theme/Palette.h"

#include <QThread>
#include <QTimer>

#include <common.h>

namespace qr {

namespace {

// The quiet zone sits on the design's blue grey, its lightness the contrast.
constexpr int BACKGROUND_HUE = 220;
constexpr qreal BACKGROUND_SATURATION = 0.14;
constexpr qreal PERCENT = 100.0;
constexpr qreal FULL_CIRCLE = 360.0;
constexpr int MAX_COLOR_VALUE = 255;

} // namespace

QrDisplayEngine::QrDisplayEngine(QObject *parent) : QObject(parent) {
    m_frame_timer = new QTimer(this);
    m_frame_timer->setInterval(static_cast<int>(m_speed));
    rebuildColorTable();
    connect(m_frame_timer, &QTimer::timeout, this, &QrDisplayEngine::onFrameTick, qontrol::UNIQUE);
    connect(this, &QrDisplayEngine::rendered, this, &QrDisplayEngine::onRendered, qontrol::UNIQUE);
}

QrDisplayEngine::~QrDisplayEngine() {
    stopWorker();
}

void QrDisplayEngine::stopWorker() {
    for (const auto &thread : m_render_threads) {
        if (!thread.isNull()) {
            thread->wait();
        }
    }
}

void QrDisplayEngine::setDensity(QrDensity density) {
    if (density == m_density) {
        return;
    }
    m_density = density;
    if (!m_payload.isEmpty()) {
        startRender();
    }
}

void QrDisplayEngine::setContrast(QrContrast contrast) {
    if (contrast == m_contrast) {
        return;
    }
    m_contrast = contrast;
    rebuildColorTable();
    emitFrame();
}

void QrDisplayEngine::setSpeed(QrSpeed speed) {
    m_speed = speed;
    m_frame_timer->setInterval(static_cast<int>(m_speed));
}

void QrDisplayEngine::stop() {
    m_playing = false;
    m_frame_timer->stop();
}

void QrDisplayEngine::restart() {
    m_playing = true;
    m_index = 0;
    emitFrame();
    if (!m_frames.isEmpty()) {
        m_frame_timer->start();
    }
}

void QrDisplayEngine::request(const QrRequest &request) {
    m_payload = request.payload;
    m_playing = true;
    startRender();
}

void QrDisplayEngine::startRender() {
    ++m_generation;
    m_frame_timer->stop();
    m_frames.clear();
    m_error.clear();
    m_index = 0;
    emitFrame();
    auto *thread = QThread::create(
        [this, payload = m_payload, density = m_density, generation = m_generation]() {
            emit rendered(generation, render(payload, density));
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    m_render_threads.removeAll(QPointer<QThread>());
    m_render_threads.append(thread);
    thread->start();
}

void QrDisplayEngine::onRendered(quint64 generation, const QrRender &render) {
    // A later render, after another request or density, supersedes this one.
    if (generation != m_generation) {
        return;
    }
    m_frames = render.frames;
    m_error = render.error;
    m_index = 0;
    emitFrame();
    if (m_playing && !m_frames.isEmpty()) {
        m_frame_timer->start();
    }
}

void QrDisplayEngine::onFrameTick() {
    if (m_frames.isEmpty()) {
        return;
    }
    m_index = (m_index + 1) % static_cast<int>(m_frames.size());
    emitFrame();
}

void QrDisplayEngine::emitFrame() {
    QrFrame out;
    out.background = background();
    out.index = m_index;
    out.count = static_cast<int>(m_frames.size());
    out.error = m_error;
    if (!m_frames.isEmpty()) {
        const QImage &frame = m_frames.at(m_index);
        QImage indexed(frame.constBits(), frame.width(), frame.height(), frame.bytesPerLine(),
                       QImage::Format_Indexed8);
        indexed.setColorTable(m_color_table);
        out.image = indexed.copy();
    }
    emit frame(out);
}

auto QrDisplayEngine::background() const -> QColor {
    return QColor::fromHslF(BACKGROUND_HUE / FULL_CIRCLE, BACKGROUND_SATURATION,
                            static_cast<int>(m_contrast) / PERCENT);
}

void QrDisplayEngine::rebuildColorTable() {
    QColor quietZone = background();
    m_color_table.resize(MAX_COLOR_VALUE + 1);
    for (int level = 0; level <= MAX_COLOR_VALUE; ++level) {
        m_color_table[level] =
            mixColor(Qt::black, quietZone, static_cast<qreal>(level) / MAX_COLOR_VALUE).rgb();
    }
}

} // namespace qr
