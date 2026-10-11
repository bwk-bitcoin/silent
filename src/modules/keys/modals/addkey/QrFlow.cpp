#include "QrFlow.h"

#include "KeyReview.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "i18n/Tr.h"
#include "modules/keys/BwkQrEngine.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/flow/CameraStep.h"
#include "qr/QrCameraEngine.h"
#include "qr/QrDisplay.h"
#include "qr/QrDisplayEngine.h"
#include "qr/bbqr/BbqrDisplayEngine.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QDebug>
#include <Qontrol>
#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

QrFlow::QrFlow(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

QrFlow::~QrFlow() {
    stop();
}

void QrFlow::start() {
    showDisplay();
    m_qr_engine->getXpub(KeysModule::defaultXpubPath());
}

void QrFlow::init() {
    m_qr_engine = new BwkQrEngine(m_module, this);
    initDisplay();
    initCamera();
    initReview();
    initFailure();
}

void QrFlow::doConnect() {
    connectDisplay();
    connectCamera();
    connectReview();
    connectFailure();
}

void QrFlow::view() {
    m_qr_step = viewDisplay();
    hideCamera();
    hideReview();
    hideFailure();
}

// ===== QR code view =====

void QrFlow::initDisplay() {
    // A plain code on white, the highest contrast, with no tuning controls.
    m_qr_display_engine = new qr::BbqrDisplayEngine(this);
    m_qr_display_engine->setContrast(qr::QrContrast::Highest);
    m_qr_hint = new Label(LabelRole::Body);
    m_qr_hint->setTone(LabelTone::Secondary);
    m_qr_hint->setAlignment(Qt::AlignCenter);
    // On one line when it fits, as the design's text takes its natural width.
    m_qr_hint->setTextWrapped(TR("keys-add-qr-hint"), metric::QR_HINT_MAX_WIDTH);
    m_qr_display = new qr::QrDisplay;
    m_qr_display->setTunable(false);
    m_qr_display->setSize(metric::QR_REQUEST_SIZE);
    m_qr_next = catalog::btnNextScanTheAnswer();
    m_qr_actions = new catalog::ModalActions;
    m_qr_actions->setBack(catalog::ModalActions::Back::Back);
    m_qr_actions->addAction(m_qr_next);
}

void QrFlow::connectDisplay() {
    connect(m_qr_display_engine, &qr::QrDisplayEngine::frame, m_qr_display, &qr::QrDisplay::frame,
            qontrol::UNIQUE);
    connect(m_qr_display_engine, &qr::QrDisplayEngine::frame, this, &QrFlow::onQrFrame,
            qontrol::UNIQUE);
    connect(m_qr_next, &QPushButton::clicked, this, &QrFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_qr_actions, &catalog::ModalActions::backClicked, this, &QrFlow::onBackClicked,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::request, m_qr_display_engine, &qr::QrDisplayEngine::request,
            qontrol::UNIQUE);
}

auto QrFlow::viewDisplay() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_qr_hint)
        ->pushAligned(m_qr_display, Qt::AlignHCenter);
}

void QrFlow::showDisplay() {
    m_view = View::Display;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_qr_step);
    viewSlots()->setFooter(m_qr_actions);
    emit viewChanged(TR("keys-add-qr-title"), ModalWidth::L);
}

// ===== Camera view =====

void QrFlow::initCamera() {
    m_camera = new flow::CameraStep;
    m_camera->setHint(TR("keys-add-scan-hint"));
}

void QrFlow::connectCamera() {
    connect(m_camera, &flow::CameraStep::backClicked, this, &QrFlow::onBackClicked,
            qontrol::UNIQUE);
    connect(m_camera->engine(), &qr::QrCameraEngine::response, m_qr_engine, &qr::QrEngine::response,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::xpub, this, &QrFlow::onQrXpub, qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::error, this, &QrFlow::onQrError, qontrol::UNIQUE);
}

void QrFlow::showCamera() {
    m_view = View::Camera;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_camera);
    viewSlots()->setFooter(m_camera->actions());
    emit viewChanged(TR("keys-add-scan-title"), ModalWidth::XXL);
}

void QrFlow::hideCamera() {
    viewSlots()->keep(m_camera);
    viewSlots()->keep(m_camera->actions());
}

// ===== Review view =====

void QrFlow::initReview() {
    m_review = new KeyReview(m_module, TR("keys-add-review-title"));
}

void QrFlow::connectReview() {
    connect(m_review, &KeyReview::addClicked, this, &QrFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_review, &KeyReview::backClicked, this, &QrFlow::onBackClicked, qontrol::UNIQUE);
}

void QrFlow::showReview() {
    m_view = View::Review;
    viewSlots()->setHeader(m_review->header());
    viewSlots()->setBody(m_review);
    viewSlots()->setFooter(m_review->actions());
    emit viewChanged(TR("keys-add-review-title"), ModalWidth::L);
}

void QrFlow::hideReview() {
    viewSlots()->keep(m_review);
    viewSlots()->keep(m_review->header());
    viewSlots()->keep(m_review->actions());
}

// ===== Failure view =====

void QrFlow::initFailure() {
    m_status = new catalog::ModalStatus;
    m_status->setPadding(resolve(Spacing::M), resolve(Spacing::M));
    m_status->setIcon(icon::circleX, IconColor::Error);
    m_status->setTitle(TR("keys-qr-failed-title"));
    m_try_again = catalog::btnTryAgain();
    m_status_actions = new catalog::ModalActions;
    m_status_actions->addAction(m_try_again);
}

void QrFlow::connectFailure() {
    connect(m_try_again, &QPushButton::clicked, this, &QrFlow::onNextClicked, qontrol::UNIQUE);
}

void QrFlow::showFailure(const QString &message) {
    m_view = View::Failure;
    m_status->setSubtitle(message);
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_status);
    viewSlots()->setFooter(m_status_actions);
    emit viewChanged(TR("keys-add-title"), ModalWidth::L);
}

void QrFlow::hideFailure() {
    viewSlots()->keep(m_status);
    viewSlots()->keep(m_status_actions);
}

// ===== Shared =====

void QrFlow::stop() {
    m_qr_engine->stop();
    m_qr_display_engine->stop();
    m_camera->stop();
}

// ===== Slots =====

void QrFlow::onBackClicked() {
    switch (m_view) {
    case View::Display:
        emit back();
        return;
    case View::Camera:
        m_camera->stop();
        showDisplay();
        m_qr_display_engine->restart();
        return;
    case View::Review:
        start();
        return;
    case View::Failure:
        return;
    }
}

void QrFlow::onNextClicked() {
    switch (m_view) {
    case View::Display:
        m_qr_display_engine->stop();
        showCamera();
        m_camera->start();
        return;
    case View::Review:
        if (m_review->add({KeyRouteKind::Qr})) {
            // The key is added either way: a device not remembered only goes unseen.
            auto recorded = m_module->recordQrDevice(m_fingerprint, m_device);
            if (!recorded.is_ok) {
                qWarning() << "keys:" << KeysModule::toQt(recorded.error);
            }
            emit added();
        }
        return;
    case View::Failure:
        start();
        return;
    case View::Camera:
        return;
    }
}

void QrFlow::onQrXpub(const QString &fingerprint, const QString &path, const QString &xpub,
                      const qr::QrDevice &device) {
    stop();
    m_fingerprint = fingerprint;
    m_device = device;
    m_review->setKey(fingerprint, {{.path = path, .xpub = xpub}}, QString());
    showReview();
}

void QrFlow::onQrError(qr::QrError kind, const QString &message) {
    if (kind != qr::QrError::RequestFailed) {
        // A refused answer: the scan goes on.
        m_camera->showScanError(kind, message);
        window()->adjustSize();
        return;
    }
    stop();
    showFailure(message);
}

void QrFlow::onQrFrame(const qr::QrFrame &frame) {
    bool rendering = frame.image.isNull() && frame.error.isEmpty();
    m_qr_next->setEnabled(!rendering);
    m_qr_actions->setReason(rendering ? TR("flow-qr-rebuild-reason") : QString());
}

} // namespace keys::modal
