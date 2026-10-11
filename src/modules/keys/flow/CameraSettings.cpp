#include "CameraSettings.h"

#include "QrDisplayStep.h"

#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Stepper.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <common.h>

namespace keys::flow {

CameraSettings::CameraSettings(const QStringList &cameras, int camera, int contrast,
                               CameraViewport::State state) {
    setWindowTitle(TR("flow-camera-settings-title"));
    setFixedWidth(resolve(ModalWidth::XS));
    init(cameras, camera, contrast);
    doConnect();
    view();
    setState(state);
}

void CameraSettings::setState(CameraViewport::State state) {
    m_viewport->setState(state);
}

void CameraSettings::setFrame(const QImage &frame) {
    m_viewport->setFrame(frame);
}

void CameraSettings::init(const QStringList &cameras, int camera, int contrast) {
    m_viewport = new CameraViewport;

    m_camera_combo = new catalog::ComboBox;
    m_camera_combo->addItems(cameras);
    m_camera_combo->setCurrentIndex(camera);
    m_camera_combo->setEnabled(!cameras.isEmpty());
    m_camera_combo->setFixedWidth(metric::CAMERA_SOURCE_WIDTH);

    m_contrast_stepper = new catalog::Stepper;
    m_contrast_stepper->setRange(QrDisplayStep::MIN_LEVEL, QrDisplayStep::MAX_LEVEL);
    m_contrast_stepper->setValue(contrast);
}

void CameraSettings::doConnect() {
    connect(m_camera_combo, &QComboBox::currentIndexChanged, this, &CameraSettings::cameraPicked,
            qontrol::UNIQUE);
    connect(m_contrast_stepper, &catalog::Stepper::valueChanged, this,
            &CameraSettings::contrastPicked, qontrol::UNIQUE);
}

void CameraSettings::view() {
    auto *fields = (new catalog::FieldGroup)
                       ->addField(TR("flow-camera-source"), m_camera_combo)
                       ->addField(TR("flow-camera-contrast"), m_contrast_stepper);
    // As wide as the fields under it, as the design's preview takes their width.
    m_viewport->setFixedWidth(fields->sizeHint().width());
    setMainWidget((new qontrol::Column)
                      ->margins(resolve(Padding::L))
                      ->spacing(resolve(Spacing::M))
                      ->pushAligned(m_viewport, Qt::AlignHCenter)
                      ->pushAligned(fields, Qt::AlignHCenter));
}

} // namespace keys::flow
