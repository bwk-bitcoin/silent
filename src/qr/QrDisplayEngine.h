#pragma once

#include "qr/QrTypes.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRgb>

#include <memory>

class QThread;
class QTimer;

namespace qr {

// A transport's framing: renders a payload into its animated frames at a density
// level. Called on worker threads, so it holds nothing an engine changes.
class QrFramer {
public:
    virtual ~QrFramer() = default;
    [[nodiscard]] virtual auto render(const QByteArray &payload, int density) const -> QrRender = 0;
};

// Plays a request as animated QR frames, protocol agnostic: the format's QrFramer
// turns the payload into images, and this engine maps them to the contrast and plays
// them at the speed. A density change renders the frames again, off the GUI thread.
// Each transport inherits it with its framer.
class QrDisplayEngine : public QObject {
    Q_OBJECT

public:
    static constexpr int BASE_INTERVAL_MS = 520;
    static constexpr int SPEED_STEP_MS = 80;

    QrDisplayEngine(std::shared_ptr<const QrFramer> framer, const QrDisplaySettings &settings,
                    QObject *parent = nullptr);
    ~QrDisplayEngine() override;
    void setSettings(const QrDisplaySettings &settings);
    [[nodiscard]] auto settings() const -> QrDisplaySettings;
    // Pauses on the frame shown; restart() plays again from the first.
    void stop();
    void restart();

signals:
    void frame(const qr::QrFrame &frame);
    // A worker's render, tagged with the generation it was started for.
    void rendered(quint64 generation, const qr::QrRender &render);

public slots:
    void request(const qr::QrRequest &request);
    void onRendered(quint64 generation, const qr::QrRender &render);
    void onFrameTick();

protected:
    void render();
    void emitFrame();
    [[nodiscard]] auto frameInterval() const -> int;
    [[nodiscard]] auto background() const -> QColor;
    // Rebuilt only when the contrast changes, so the per-frame path never allocates.
    void rebuildColorTable();

private:
    std::shared_ptr<const QrFramer> m_framer;
    QrDisplaySettings m_settings;
    QByteArray m_payload;
    QList<QImage> m_frames;
    QString m_error;
    // Grayscale level to quiet-zone colour, for the current contrast.
    QList<QRgb> m_color_table;
    QTimer *m_frame_timer = nullptr;
    // Waited for on destruction: a worker emits on this engine.
    QList<QPointer<QThread>> m_render_threads;
    quint64 m_generation = 0;
    int m_index = 0;
    bool m_playing = false;
};

} // namespace qr
