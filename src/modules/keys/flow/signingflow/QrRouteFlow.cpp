#include "QrRouteFlow.h"

#include "VerifyResultView.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/Separator.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Stepper.h"
#include "i18n/Tr.h"
#include "modules/keys/BwkQrEngine.h"
#include "modules/keys/flow/CameraStep.h"
#include "qr/QrCameraEngine.h"
#include "qr/QrDisplay.h"
#include "qr/QrDisplayEngine.h"
#include "qr/bbqr/BbqrDisplayEngine.h"
#include "theme/Palette.h"

#include <QDebug>
#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::flow {

namespace {

// One tuning control of the QR view: the stepper over the line that says what it does.
auto tuneColumn(catalog::Stepper *stepper, const QString &hint) -> QWidget * {
    auto *caption = new catalog::Label(hint, catalog::LabelRole::Caption);
    caption->setWordWrap(true);
    caption->setAlignment(Qt::AlignCenter);
    auto *column = (new qontrol::Column)
                       ->spacing(resolve(Spacing::XXS))
                       ->pushAligned(stepper, Qt::AlignHCenter)
                       ->push(caption);
    column->setFixedWidth(resolve(Size::S));
    return column;
}

auto levelStepper(const QString &label) -> catalog::Stepper * {
    auto *stepper = new catalog::Stepper;
    stepper->setLabel(label);
    stepper->setRange(qr::MIN_LEVEL, qr::MAX_LEVEL);
    stepper->setValue(qr::DEFAULT_LEVEL);
    return stepper;
}

} // namespace

QrRouteFlow::QrRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row,
                         QString psbt)
    : SigningRouteFlow(module, std::move(request), std::move(row), std::move(psbt)) {
    init();
    doConnect();
    view();
}

QrRouteFlow::~QrRouteFlow() {
    stop();
}

void QrRouteFlow::start() {
    stop();
    showDisplay();
    switch (request().context) {
    case Context::Sign:
        m_qr_engine->sign(psbt(), request().descriptor);
        return;
    case Context::Verify:
        m_qr_engine->verifyAddress(request().descriptor, request().path, request().address);
        return;
    case Context::Fetch:
        m_qr_engine->getSpDescriptor();
        return;
    }
}

void QrRouteFlow::init() {
    m_qr_engine = new BwkQrEngine(module(), this);
    initDisplay();
    initCamera();
    initVerifyResult();
}

void QrRouteFlow::doConnect() {
    connectDisplay();
    connectCamera();
    connectVerifyResult();
}

void QrRouteFlow::view() {
    m_qr_step = viewDisplay();
    hideCamera();
    hideVerifyResult();
}

// ===== QR code view =====

void QrRouteFlow::initDisplay() {
    m_qr_display_engine = new qr::BbqrDisplayEngine(this);
    m_qr_hint = new catalog::Label(catalog::LabelRole::Body);
    m_qr_hint->setTone(catalog::LabelTone::Secondary);
    m_qr_hint->setAlignment(Qt::AlignCenter);
    // On one line when it fits, as the design's text takes its natural width.
    m_qr_hint->setTextWrapped(copy().qrHint, metric::QR_HINT_MAX_WIDTH);
    m_qr_display = new qr::QrDisplay;
    m_qr_contrast = levelStepper(TR("keys-qr-contrast"));
    m_qr_density = levelStepper(TR("keys-qr-density"));
    m_qr_speed = levelStepper(TR("flow-qr-speed"));
    m_qr_next = catalog::btnNextScanTheAnswer();
    m_qr_actions = new catalog::ModalActions;
    m_qr_actions->addAction(m_qr_next);
}

void QrRouteFlow::connectDisplay() {
    connect(m_qr_display_engine, &qr::QrDisplayEngine::frame, m_qr_display, &qr::QrDisplay::frame,
            qontrol::UNIQUE);
    connect(m_qr_display_engine, &qr::QrDisplayEngine::frame, this, &QrRouteFlow::onQrFrame,
            qontrol::UNIQUE);
    for (auto *stepper : {m_qr_contrast, m_qr_density, m_qr_speed}) {
        connect(stepper, &catalog::Stepper::valueChanged, this, &QrRouteFlow::onQrSettingsChanged,
                qontrol::UNIQUE);
    }
    connect(m_qr_next, &QPushButton::clicked, this, &QrRouteFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::request, m_qr_display_engine, &qr::QrDisplayEngine::request,
            qontrol::UNIQUE);
}

auto QrRouteFlow::viewDisplay() -> QWidget * {
    auto *tuning =
        (new qontrol::Row)
            ->spacing(resolve(Spacing::L))
            ->pushSpacer()
            ->pushAligned(tuneColumn(m_qr_contrast, TR("flow-qr-contrast-hint")), Qt::AlignTop)
            ->pushAligned(tuneColumn(m_qr_density, TR("flow-qr-density-hint")), Qt::AlignTop)
            ->pushAligned(tuneColumn(m_qr_speed, TR("flow-qr-speed-hint")), Qt::AlignTop)
            ->pushSpacer();
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_qr_hint)
        ->push(m_qr_display)
        ->push(new catalog::Separator)
        ->push(tuning);
}

void QrRouteFlow::showDisplay() {
    m_view = View::Display;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_qr_step);
    viewSlots()->setFooter(m_qr_actions);
    emit viewChanged(copy().qrTitle, ModalWidth::XXL);
}

// ===== Camera view =====

void QrRouteFlow::initCamera() {
    m_camera = new CameraStep;
    m_camera->setHint(copy().scanHint);
}

void QrRouteFlow::connectCamera() {
    connect(m_camera, &CameraStep::backClicked, this, &QrRouteFlow::onBackClicked, qontrol::UNIQUE);
    connect(m_camera->engine(), &qr::QrCameraEngine::response, m_qr_engine, &qr::QrEngine::response,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::signedPsbt, this, &QrRouteFlow::onQrSigned,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::addressVerified, this, &QrRouteFlow::onQrVerified,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::spDescriptor, this, &QrRouteFlow::onQrDescriptor,
            qontrol::UNIQUE);
    connect(m_qr_engine, &qr::QrEngine::error, this, &QrRouteFlow::onQrError, qontrol::UNIQUE);
}

void QrRouteFlow::showCamera() {
    m_view = View::Camera;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_camera);
    viewSlots()->setFooter(m_camera->actions());
    emit viewChanged(copy().scanTitle, ModalWidth::XXL);
}

void QrRouteFlow::hideCamera() {
    viewSlots()->keep(m_camera);
    viewSlots()->keep(m_camera->actions());
}

// ===== Verify result view =====

void QrRouteFlow::initVerifyResult() {
    m_result = new VerifyResultView(request().address);
}

void QrRouteFlow::connectVerifyResult() {
    connect(m_result, &VerifyResultView::retryClicked, this, &QrRouteFlow::onNextClicked,
            qontrol::UNIQUE);
}

void QrRouteFlow::showVerifyResult(SigningFlow::VerifyResult result) {
    m_view = View::VerifyResult;
    emit verifyResultChanged(result);
    m_result->setResult(result, deviceLabel());
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_result);
    // A match needs no Try Again.
    viewSlots()->setFooter(result == SigningFlow::VerifyResult::Matched ? nullptr
                                                                        : m_result->actions());
    emit viewChanged(copy().title, ModalWidth::S);
}

void QrRouteFlow::hideVerifyResult() {
    viewSlots()->keep(m_result);
    viewSlots()->keep(m_result->actions());
}

// ===== Shared =====

void QrRouteFlow::stop() {
    m_qr_engine->stop();
    m_qr_display_engine->stop();
    m_camera->stop();
}

// ===== Slots =====

void QrRouteFlow::onBackClicked() {
    switch (m_view) {
    case View::Camera:
        m_camera->stop();
        showDisplay();
        m_qr_display_engine->restart();
        return;
    case View::Display:
    case View::VerifyResult:
        return;
    }
}

void QrRouteFlow::onNextClicked() {
    switch (m_view) {
    case View::Display:
        m_qr_display_engine->stop();
        showCamera();
        m_camera->start();
        return;
    case View::VerifyResult:
        emit verifyResultChanged(std::nullopt);
        start();
        return;
    case View::Camera:
        return;
    }
}

void QrRouteFlow::onQrSigned(const QString &psbt_v2_hex) {
    m_camera->stop();
    emit psbtSigned(psbt_v2_hex);
}

void QrRouteFlow::onQrVerified(bool matched) {
    m_camera->stop();
    showVerifyResult(matched ? SigningFlow::VerifyResult::Matched
                             : SigningFlow::VerifyResult::Mismatch);
}

void QrRouteFlow::onQrDescriptor(const QString &descriptor) {
    m_camera->stop();
    emit descriptorFetched(descriptor);
}

void QrRouteFlow::onQrError(qr::QrError kind, const QString &message) {
    if (kind != qr::QrError::RequestFailed) {
        // A refused answer: the scan goes on.
        m_camera->showScanError(kind, message);
        window()->adjustSize();
        return;
    }
    m_camera->stop();
    if (request().context == Context::Verify) {
        qWarning() << "keys: qr request:" << message;
        showVerifyResult(SigningFlow::VerifyResult::Error);
        return;
    }
    emit failed(message);
}

void QrRouteFlow::onQrSettingsChanged() {
    m_qr_display_engine->setDensity(qr::densityFromLevel(m_qr_density->value()));
    m_qr_display_engine->setContrast(qr::contrastFromLevel(m_qr_contrast->value()));
    m_qr_display_engine->setSpeed(qr::speedFromLevel(m_qr_speed->value()));
}

void QrRouteFlow::onQrFrame(const qr::QrFrame &frame) {
    bool rendering = frame.image.isNull() && frame.error.isEmpty();
    m_qr_density->setEnabled(!rendering);
    m_qr_next->setEnabled(!rendering);
    m_qr_actions->setReason(rendering ? TR("flow-qr-rebuild-reason") : QString());
}

} // namespace keys::flow
