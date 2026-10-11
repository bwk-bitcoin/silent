#include "qr/QrCamera.h"

#include <QDebug>
#include <QImage>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <string>

#include <common.h>

namespace qr {

namespace {

constexpr int RGB_CHANNELS = 3;

auto toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

} // namespace

QrCamera::QrCamera(QObject *parent) : QObject(parent) {
    m_poll_timer = new QTimer(this);
    m_poll_timer->setInterval(POLL_INTERVAL_MS);
    connect(m_poll_timer, &QTimer::timeout, this, &QrCamera::onPollTick, qontrol::UNIQUE);
    connect(this, &QrCamera::opened, this, &QrCamera::onOpened, qontrol::UNIQUE);
}

QrCamera::~QrCamera() {
    stop();
}

void QrCamera::start() {
    m_names.clear();
    m_indexes.clear();
    auto list = ::camera_list();
    if (!list.is_ok) {
        qWarning() << "camera list:" << toQt(list.error);
        stop();
        setState(State::Denied);
        return;
    }
    if (list.names.empty()) {
        stop();
        setState(State::None);
        return;
    }
    for (std::size_t i = 0; i < list.names.size(); ++i) {
        m_names.append(toQt(list.names[i]));
        m_indexes.append(list.indexes[i]);
    }
    open(std::clamp(m_camera, 0, static_cast<int>(m_indexes.size()) - 1));
}

void QrCamera::open(int camera) {
    if (camera < 0 || camera >= m_indexes.size()) {
        return;
    }
    stop();
    m_camera = camera;
    setState(State::Starting);
    quint64 attempt = m_attempt;
    std::uint32_t index = m_indexes.at(camera);
    auto *thread = QThread::create(
        [this, attempt, index]() { emit opened(attempt, ::camera_open(index).into_raw()); });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void QrCamera::stop() {
    // An open still running lands on a stale attempt and is dropped.
    ++m_attempt;
    m_poll_timer->stop();
    m_handle.reset();
}

auto QrCamera::cameras() const -> QStringList {
    return m_names;
}

auto QrCamera::camera() const -> int {
    return m_camera;
}

auto QrCamera::state() const -> State {
    return m_state;
}

void QrCamera::onOpened(quint64 attempt, CameraHandle *handle) {
    auto box = rust::Box<CameraHandle>::from_raw(handle);
    if (attempt != m_attempt) {
        // Dropping `box` releases the device.
        return;
    }
    if (!box->is_ok()) {
        qWarning() << "camera open:" << toQt(box->get_error());
        setState(State::Failed);
        return;
    }
    m_handle = std::move(box);
    m_last_seq = 0;
    m_poll_timer->start();
}

void QrCamera::onPollTick() {
    if (!m_handle.has_value()) {
        return;
    }
    auto failure = toQt(m_handle.value()->failure());
    if (!failure.isEmpty()) {
        qWarning() << "camera failed:" << failure;
        stop();
        setState(State::Failed);
        return;
    }
    auto captured = m_handle.value()->frame();
    if (!captured.is_ok || captured.seq == m_last_seq) {
        return;
    }
    m_last_seq = captured.seq;
    setState(State::Live);
    auto width = static_cast<int>(captured.width);
    QImage image = QImage(captured.data.data(), width, static_cast<int>(captured.height),
                          width * RGB_CHANNELS, QImage::Format_RGB888)
                       .copy();
    emit frame(CameraFrame{.image = image, .seq = captured.seq});
}

void QrCamera::setState(State state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

} // namespace qr
