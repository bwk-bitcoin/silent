#include "CameraStep.h"

#include "CameraSettings.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/feedback/ScanProgress.h"
#include "i18n/Tr.h"
#include "qr/QrCameraDisplay.h"
#include "qr/bbqr/BbqrCameraEngine.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace keys::flow {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

CameraStep::CameraStep(QWidget *parent, qr::PlainCodes plain) : QWidget(parent) {
    init(plain);
    doConnect();
    view();
}

void CameraStep::init(qr::PlainCodes plain) {
    m_camera = new qr::QrCamera(this);
    m_engine = new qr::BbqrCameraEngine(this, plain);
    // The contrast picked last in Camera Settings, for any account.
    m_engine->setContrast(qr::cameraContrastFromLevel(::app_qr_camera_contrast_level()));

    m_hint = new Label(LabelRole::Body);
    m_hint->setTone(LabelTone::Secondary);
    m_hint->setWordWrap(true);

    m_display = new qr::QrCameraDisplay;
    m_progress = new catalog::ScanProgress;
    m_progress->setVisible(false);

    m_scan_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_scan_again = catalog::btnScanAgain();
    m_scan_error->setAction(m_scan_again);
    m_scan_error->setVisible(false);

    m_settings = catalog::btnCameraSettings();
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::Back);
    m_actions->addAction(m_settings);
}

void CameraStep::doConnect() {
    connect(m_camera, &qr::QrCamera::frame, m_engine, &qr::QrCameraEngine::frame, qontrol::UNIQUE);
    connect(m_engine, &qr::QrCameraEngine::broadcast, m_display, &qr::QrCameraDisplay::frame,
            qontrol::UNIQUE);
    connect(m_camera, &qr::QrCamera::stateChanged, this, &CameraStep::onCameraState,
            qontrol::UNIQUE);
    connect(m_engine, &qr::QrCameraEngine::progress, this, &CameraStep::onProgress,
            qontrol::UNIQUE);
    connect(m_scan_again, &QPushButton::clicked, this, &CameraStep::onScanAgainClicked,
            qontrol::UNIQUE);
    connect(m_settings, &QPushButton::clicked, this, &CameraStep::onSettingsClicked,
            qontrol::UNIQUE);
    connect(m_actions, &catalog::ModalActions::backClicked, this, &CameraStep::backClicked,
            qontrol::UNIQUE);
}

void CameraStep::view() {
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_hint)
        ->push(m_display)
        ->push(m_progress)
        ->push(m_scan_error)
        ->into(this);
}

void CameraStep::setHint(const QString &hint) {
    m_hint->setText(hint);
}

void CameraStep::start() {
    m_scan_error->setVisible(false);
    m_progress->setProgress(0, 0);
    m_progress->setVisible(false);
    m_engine->reset();
    m_camera->start();
    onCameraState(m_camera->state());
}

void CameraStep::stop() {
    m_camera->stop();
}

void CameraStep::showScanError(qr::QrError kind, const QString &message) {
    switch (kind) {
    case qr::QrError::WrongRequest:
        m_scan_error->setTitle(TR("flow-scan-wrong-request-title"));
        m_scan_error->setText(TR("flow-scan-wrong-request-body"));
        break;
    case qr::QrError::Unsupported:
        m_scan_error->setTitle(TR("flow-scan-bare-sigs-title"));
        m_scan_error->setText(TR("flow-scan-bare-sigs-body"));
        break;
    case qr::QrError::RequestFailed:
    case qr::QrError::Refused:
    case qr::QrError::MusigSession:
    default:
        m_scan_error->setTitle(TR("flow-scan-other-title"));
        m_scan_error->setText(message);
        break;
    }
    m_scan_error->setVisible(true);
}

auto CameraStep::engine() const -> qr::QrCameraEngine * {
    return m_engine;
}

auto CameraStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void CameraStep::onCameraState(qr::QrCamera::State state) {
    m_display->setState(state);
    m_progress->setVisible(state == qr::QrCamera::State::Live);
}

void CameraStep::onProgress(int seen, int total) {
    m_progress->setProgress(seen, total);
}

void CameraStep::onSettingsClicked() {
    AppController::execModal(new CameraSettings(m_camera, m_engine));
}

void CameraStep::onScanAgainClicked() {
    m_scan_error->setVisible(false);
    m_progress->setProgress(0, 0);
    m_engine->reset();
}

} // namespace keys::flow
