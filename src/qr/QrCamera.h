#pragma once

#include "qr/QrTypes.h"

#include <QList>
#include <QObject>
#include <QStringList>

#include <cstdint>
#include <optional>

#include <silent.h>

class QTimer;

namespace qr {

// A camera handing out bare frames: lists the cameras, opens one off the GUI thread,
// and emits each new picture it captures.
class QrCamera : public QObject {
    Q_OBJECT

public:
    enum class State : std::uint8_t { Starting, None, Denied, Failed, Live };

    static constexpr int POLL_INTERVAL_MS = 33;

    explicit QrCamera(QObject *parent = nullptr);
    ~QrCamera() override;
    // Lists the cameras and opens the one picked last.
    void start();
    // `camera` indexes cameras().
    void open(int camera);
    void stop();
    [[nodiscard]] auto cameras() const -> QStringList;
    [[nodiscard]] auto camera() const -> int;
    [[nodiscard]] auto state() const -> State;

signals:
    void frame(const qr::CameraFrame &frame);
    void stateChanged(qr::QrCamera::State state);
    // The opened camera's raw pointer, re-owned by onOpened().
    void opened(quint64 attempt, CameraHandle *handle);

public slots:
    void onOpened(quint64 attempt, CameraHandle *handle);
    void onPollTick();

protected:
    void setState(State state);

private:
    std::optional<rust::Box<CameraHandle>> m_handle;
    QTimer *m_poll_timer = nullptr;
    QStringList m_names;
    QList<std::uint32_t> m_indexes;
    int m_camera = 0;
    // The open() made last; an older one's handle is dropped as it lands.
    quint64 m_attempt = 0;
    quint64 m_last_seq = 0;
    State m_state = State::Starting;
};

} // namespace qr
