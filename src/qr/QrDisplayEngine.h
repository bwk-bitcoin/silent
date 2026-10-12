#pragma once

#include "qr/QrTypes.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRgb>

class QThread;
class QTimer;

namespace qr {

// Plays a request as animated QR frames, protocol agnostic: the format's render()
// turns the payload into images, and this engine maps them to the contrast and plays
// them at the speed. A density change renders the frames again, off the GUI thread.
// Each transport inherits it and implements render().
//
// render() runs on worker threads, so every subclass destructor calls stopWorker()
// first: once the subclass is gone a worker would call a pure virtual.
class QrDisplayEngine : public QObject {
    Q_OBJECT

public:
    // Every setting starts at its Medium.
    explicit QrDisplayEngine(QObject *parent = nullptr);
    ~QrDisplayEngine() override;
    // A density change renders the frames again; contrast and speed apply at once.
    void setDensity(QrDensity density);
    void setContrast(QrContrast contrast);
    void setSpeed(QrSpeed speed);
    // Pauses on the frame shown; restart() plays again from the first.
    void stop();
    void restart();
    // Emits the frame on display again, for a display connected after it was shown.
    void refresh();

signals:
    void frame(const qr::QrFrame &frame);
    // A worker's render, tagged with the generation it was started for.
    void rendered(quint64 generation, const qr::QrRender &render);

public slots:
    void request(const qr::QrRequest &request);
    void onRendered(quint64 generation, const qr::QrRender &render);
    void onFrameTick();

protected:
    // Renders a payload into its frames at a density. Called on worker threads, so it
    // reads nothing the engine changes.
    [[nodiscard]] virtual auto render(const QByteArray &payload, QrDensity density) const
        -> QrRender = 0;
    void stopWorker();
    void startRender();
    void emitFrame();
    [[nodiscard]] auto background() const -> QColor;
    // Rebuilt only when the contrast changes, so the per-frame path never allocates.
    void rebuildColorTable();

private:
    QrDensity m_density = QrDensity::Medium;
    QrContrast m_contrast = QrContrast::Medium;
    QrSpeed m_speed = QrSpeed::Medium;
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
