#pragma once

#include <Qontrol>

namespace catalog {
class ComboBox;
class Stepper;
} // namespace catalog

namespace qr {
class QrCamera;
class QrCameraDisplay;
class QrCameraEngine;
} // namespace qr

namespace keys::flow {

// The one small modal allowed over a flow: picks the camera and the scan contrast,
// applied at once while the scan keeps running behind it, with the frames the scan
// reads so the change is seen here.
class CameraSettings final : public qontrol::Modal {
    Q_OBJECT

public:
    // Non-owning: both belong to the scan step this opens over.
    CameraSettings(qr::QrCamera *camera, qr::QrCameraEngine *engine);

public slots:
    void onContrastPicked(int level);

protected:
    void init();
    void doConnect();
    void view();

private:
    qr::QrCamera *m_camera = nullptr;
    qr::QrCameraEngine *m_engine = nullptr;
    qr::QrCameraDisplay *m_display = nullptr;
    catalog::ComboBox *m_camera_combo = nullptr;
    catalog::Stepper *m_contrast_stepper = nullptr;
};

} // namespace keys::flow
