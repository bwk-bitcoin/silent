#pragma once

#include <optional>

#include <QList>
#include <QRgb>
#include <QString>
#include <QWidget>

#include <silent.h>

class QImage;
class QTimer;

namespace catalog {
class Button;
class Label;
class ModalActions;
class QrCode;
class Stepper;
} // namespace catalog

namespace keys::flow {

// Shows a QR request to an airgapped device one frame at a time, with contrast,
// density and speed controls. Ignorant of what the request asks: on a density change
// it asks its owner (densityChanged) for a request rebuilt at that density, and shows
// "Rebuilding" until setRequest() hands it over.
class QrDisplayStep : public QWidget {
    Q_OBJECT

public:
    static constexpr int MIN_LEVEL = 1;
    static constexpr int MAX_LEVEL = 5;
    static constexpr int DEFAULT_LEVEL = 3;
    static constexpr int BASE_INTERVAL_MS = 520;
    static constexpr int SPEED_STEP_MS = 80;

    explicit QrDisplayStep(QWidget *parent = nullptr);
    void setHint(const QString &hint);
    // Called directly by the owner, not as a slot: rust::Box is move only.
    void setRequest(rust::Box<QrRequest> request);
    // Dims the code until the next setRequest().
    void setRebuilding();
    // Plays the held request again from its first frame.
    void restart();
    void stop();
    [[nodiscard]] auto density() const -> int;
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void densityChanged(int density);
    void nextClicked();

public slots:
    void onFrameTick();
    void onContrastChanged(int value);
    void onDensityChanged(int value);
    void onSpeedChanged(int value);
    void onNextClicked();

protected:
    void init();
    void doConnect();
    void view();
    auto showFrame(int index) -> bool;
    [[nodiscard]] auto frameInterval() const -> int;
    [[nodiscard]] auto background() const -> QColor;
    [[nodiscard]] auto applyContrast(const QImage &frame) const -> QImage;
    // Rebuilt only when the contrast changes, so the per-frame path never allocates.
    void rebuildColorTable();
    void setRebuildingShown(bool rebuilding);

private:
    std::optional<rust::Box<QrRequest>> m_request = std::nullopt;
    QTimer *m_frame_timer = nullptr;
    catalog::Label *m_hint = nullptr;
    catalog::QrCode *m_qr_code = nullptr;
    QWidget *m_rebuilding = nullptr;
    catalog::Label *m_part_label = nullptr;
    catalog::Stepper *m_contrast_stepper = nullptr;
    catalog::Stepper *m_density_stepper = nullptr;
    catalog::Stepper *m_speed_stepper = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_next = nullptr;
    // Grayscale level to quiet-zone colour, for the current contrast.
    QList<QRgb> m_color_table;
    int m_frame_index = 0;
    int m_contrast = DEFAULT_LEVEL;
    int m_density = DEFAULT_LEVEL;
    int m_speed = DEFAULT_LEVEL;
};

} // namespace keys::flow
