#include "CameraSettings.h"

#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Stepper.h"
#include "i18n/Tr.h"
#include "qr/QrCamera.h"
#include "qr/QrCameraDisplay.h"
#include "qr/QrCameraEngine.h"
#include "theme/Palette.h"

#include <common.h>

namespace keys::flow {

CameraSettings::CameraSettings(qr::QrCamera *camera, qr::QrCameraEngine *engine)
    : m_camera(camera),
      m_engine(engine) {
    setWindowTitle(TR("flow-camera-settings-title"));
    setFixedWidth(resolve(ModalWidth::XS));
    init();
    doConnect();
    view();
}

void CameraSettings::init() {
    m_display = new qr::QrCameraDisplay;
    m_display->setState(m_camera->state());

    m_camera_combo = new catalog::ComboBox;
    m_camera_combo->addItems(m_camera->cameras());
    m_camera_combo->setCurrentIndex(m_camera->camera());
    m_camera_combo->setEnabled(!m_camera->cameras().isEmpty());
    m_camera_combo->setFixedWidth(metric::CAMERA_SOURCE_WIDTH);

    m_contrast_stepper = new catalog::Stepper;
    m_contrast_stepper->setRange(qr::MIN_LEVEL, qr::MAX_LEVEL);
    m_contrast_stepper->setValue(qr::cameraContrastLevel(m_engine->contrast()));
}

void CameraSettings::doConnect() {
    connect(m_engine, &qr::QrCameraEngine::broadcast, m_display, &qr::QrCameraDisplay::frame,
            qontrol::UNIQUE);
    connect(m_camera, &qr::QrCamera::stateChanged, m_display, &qr::QrCameraDisplay::setState,
            qontrol::UNIQUE);
    connect(m_camera_combo, &QComboBox::currentIndexChanged, m_camera, &qr::QrCamera::open,
            qontrol::UNIQUE);
    connect(m_contrast_stepper, &catalog::Stepper::valueChanged, this,
            &CameraSettings::onContrastPicked, qontrol::UNIQUE);
}

void CameraSettings::view() {
    auto *fields = (new catalog::FieldGroup)
                       ->addField(TR("flow-camera-source"), m_camera_combo)
                       ->addField(TR("flow-camera-contrast"), m_contrast_stepper);
    // As wide as the fields under it, as the design's preview takes their width.
    m_display->setWidth(fields->sizeHint().width());
    setMainWidget((new qontrol::Column)
                      ->margins(resolve(Padding::L))
                      ->spacing(resolve(Spacing::M))
                      ->pushAligned(m_display, Qt::AlignHCenter)
                      ->pushAligned(fields, Qt::AlignHCenter));
}

void CameraSettings::onContrastPicked(int level) {
    m_engine->setContrast(qr::cameraContrastFromLevel(level));
}

} // namespace keys::flow
