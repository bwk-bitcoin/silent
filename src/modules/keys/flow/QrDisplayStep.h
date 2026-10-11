#pragma once

#include <QString>
#include <QWidget>

namespace catalog {
class Button;
class Label;
class ModalActions;
class Stepper;
} // namespace catalog

namespace qr {
class QrDisplay;
class QrDisplayEngine;
struct QrFrame;
} // namespace qr

namespace keys::flow {

// Shows a QR request to an airgapped device one frame at a time, with contrast,
// density and speed controls, or as a plain code without them (setTunable). Ignorant
// of what the request asks: its codec hands the payload to engine().
class QrDisplayStep : public QWidget {
    Q_OBJECT

public:
    explicit QrDisplayStep(QWidget *parent = nullptr);
    void setHint(const QString &hint);
    // False shows the plain white code without the part count and the tuning controls.
    void setTunable(bool tunable);
    // Plays the request again from its first frame.
    void restart();
    void stop();
    [[nodiscard]] auto engine() const -> qr::QrDisplayEngine *;
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void nextClicked();

public slots:
    void onSettingsChanged();
    void onFrame(const qr::QrFrame &frame);
    void onNextClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    qr::QrDisplayEngine *m_engine = nullptr;
    catalog::Label *m_hint = nullptr;
    qr::QrDisplay *m_display = nullptr;
    QWidget *m_separator = nullptr;
    QWidget *m_tuning = nullptr;
    catalog::Stepper *m_contrast_stepper = nullptr;
    catalog::Stepper *m_density_stepper = nullptr;
    catalog::Stepper *m_speed_stepper = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_next = nullptr;
};

} // namespace keys::flow
