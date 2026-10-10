#pragma once

#include <QStringList>

#include <Qontrol>

namespace catalog {
class ComboBox;
class Stepper;
} // namespace catalog

namespace keys::flow {

// The one small modal allowed over a flow: picks the camera and the preview contrast,
// applied at once while the scan keeps running behind it.
class CameraSettings final : public qontrol::Modal {
    Q_OBJECT

public:
    CameraSettings(const QStringList &cameras, int camera, int contrast);

signals:
    void cameraPicked(int camera);
    void contrastPicked(int contrast);

protected:
    void init(const QStringList &cameras, int camera, int contrast);
    void doConnect();
    void view();

private:
    catalog::ComboBox *m_camera_combo = nullptr;
    catalog::Stepper *m_contrast_stepper = nullptr;
};

} // namespace keys::flow
