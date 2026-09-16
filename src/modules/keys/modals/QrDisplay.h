#pragma once

#include <optional>

#include <QList>
#include <QRgb>
#include <QString>

#include <Qontrol>
#include <silent.h>

class QImage;
class QTimer;

namespace catalog {
class Button;
class Label;
class QrCode;
class Stepper;
} // namespace catalog

namespace keys::modal {

// Shows a Rust built QrRequest to an airgapped device, one frame at a time, animated
// on a timer, with contrast/density/speed controls. Deliberately ignorant of which of
// the five request kinds it holds: when density changes it asks its owner (through
// densityChanged) for a freshly built request rather than rebuilding one itself.
class QrDisplay final : public qontrol::Modal {
    Q_OBJECT

public:
    static constexpr int MIN_LEVEL = 1;
    static constexpr int MAX_LEVEL = 5;
    static constexpr int DEFAULT_LEVEL = 3;
    static constexpr int BASE_INTERVAL_MS = 520;
    static constexpr int SPEED_STEP_MS = 80;

    QrDisplay(const QString &title, rust::Box<QrRequest> request, QWidget *parent = nullptr);
    ~QrDisplay() override = default;

    // Swaps in a request rebuilt at a new density. Called directly by the owner from
    // its own onDensityChanged slot, not connected as a slot: rust::Box is move only.
    void setRequest(rust::Box<QrRequest> request);
    // Resumes the animation of the already-held request from its first frame.
    // Called directly by the owner when the camera scan step closes without a new
    // request (Previous), unlike setRequest() which swaps in a rebuilt one.
    void restartAnimation();
    [[nodiscard]] auto density() const -> int;

signals:
    void densityChanged(int density);
    void nextRequested();

public slots:
    void onFrameTick();
    void onContrastChanged(int value);
    void onDensityChanged(int value);
    void onSpeedChanged(int value);
    void onNextClicked();
    void onCancelClicked();

protected:
    void init();
    void doConnect();
    void view();
    auto showFrame(int index) -> bool;
    void updatePartLabel();
    [[nodiscard]] auto frameInterval() const -> int;
    [[nodiscard]] auto applyContrast(const QImage &frame) const -> QImage;
    // Rebuilds m_color_table from m_contrast. Called at construction and whenever
    // the contrast stepper changes, never from showFrame()'s per-tick path.
    void rebuildColorTable();

private:
    QString m_title;
    std::optional<rust::Box<QrRequest>> m_request = std::nullopt;
    QTimer *m_frame_timer = nullptr;
    catalog::QrCode *m_qr_code = nullptr;
    catalog::Label *m_part_label = nullptr;
    catalog::Stepper *m_contrast_stepper = nullptr;
    catalog::Stepper *m_density_stepper = nullptr;
    catalog::Stepper *m_speed_stepper = nullptr;
    catalog::Button *m_next_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    // The grayscale-to-background color table for the current contrast level,
    // rebuilt only on contrast change so showFrame()'s per-tick path never allocates.
    QList<QRgb> m_color_table;
    int m_frame_index = 0;
    int m_contrast = DEFAULT_LEVEL;
    int m_density = DEFAULT_LEVEL;
    int m_speed = DEFAULT_LEVEL;
};

} // namespace keys::modal
