#pragma once

#include "catalog/containers/Modal.h"

namespace catalog {
class Stepper;
} // namespace catalog

namespace qr {
class QrDisplay;
class QrDisplayEngine;
struct QrFrame;
} // namespace qr

namespace keys::flow {

// Tunes an animated QR code while it keeps playing, shown above the controls as on the
// step it was opened from: contrast, density and speed, applied at once and kept for
// the next code. Opened over a QR display step, as Camera Settings is over a scan.
class QrSettings final : public catalog::Modal {
    Q_OBJECT

public:
    // Non-owning: the engine belongs to the display step this opens over.
    explicit QrSettings(qr::QrDisplayEngine *engine);

public slots:
    void onLevelChanged();
    void onFrame(const qr::QrFrame &frame);

protected:
    void init();
    void doConnect();
    void view();

private:
    qr::QrDisplayEngine *m_engine = nullptr;
    qr::QrDisplay *m_display = nullptr;
    catalog::Stepper *m_contrast = nullptr;
    catalog::Stepper *m_density = nullptr;
    catalog::Stepper *m_speed = nullptr;
};

// The levels last picked, for any account, applied to `engine`.
void applyQrLevels(qr::QrDisplayEngine *engine);

} // namespace keys::flow
