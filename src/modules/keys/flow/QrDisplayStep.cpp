#include "QrDisplayStep.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Stepper.h"
#include "i18n/Tr.h"
#include "qr/QrDisplay.h"
#include "qr/bbqr/BbqrDisplayEngine.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace keys::flow {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;
using catalog::Stepper;

namespace {

// One tuning control: the stepper over the line that says what it does.
auto tuneColumn(Stepper *stepper, const QString &hint) -> QWidget * {
    auto *caption = new Label(hint, LabelRole::Caption);
    caption->setWordWrap(true);
    caption->setAlignment(Qt::AlignCenter);
    auto *column = (new qontrol::Column)
                       ->spacing(resolve(Spacing::XXS))
                       ->pushAligned(stepper, Qt::AlignHCenter)
                       ->push(caption);
    column->setFixedWidth(resolve(Size::S));
    return column;
}

auto levelStepper(const QString &label) -> Stepper * {
    auto *stepper = new Stepper;
    stepper->setLabel(label);
    stepper->setRange(qr::MIN_LEVEL, qr::MAX_LEVEL);
    stepper->setValue(qr::DEFAULT_LEVEL);
    return stepper;
}

} // namespace

QrDisplayStep::QrDisplayStep(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
}

void QrDisplayStep::init() {
    m_engine = new qr::BbqrDisplayEngine(qr::QrDisplaySettings{}, this);

    m_hint = new Label(LabelRole::Body);
    m_hint->setTone(LabelTone::Secondary);
    m_hint->setWordWrap(true);
    m_hint->setAlignment(Qt::AlignCenter);

    m_display = new qr::QrDisplay;

    m_contrast_stepper = levelStepper(TR("keys-qr-contrast"));
    m_density_stepper = levelStepper(TR("keys-qr-density"));
    m_speed_stepper = levelStepper(TR("flow-qr-speed"));

    m_next = catalog::btnNextScanTheAnswer();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_next);
}

void QrDisplayStep::doConnect() {
    connect(m_engine, &qr::QrDisplayEngine::frame, m_display, &qr::QrDisplay::frame,
            qontrol::UNIQUE);
    connect(m_engine, &qr::QrDisplayEngine::frame, this, &QrDisplayStep::onFrame, qontrol::UNIQUE);
    connect(m_contrast_stepper, &Stepper::valueChanged, this, &QrDisplayStep::onSettingsChanged,
            qontrol::UNIQUE);
    connect(m_density_stepper, &Stepper::valueChanged, this, &QrDisplayStep::onSettingsChanged,
            qontrol::UNIQUE);
    connect(m_speed_stepper, &Stepper::valueChanged, this, &QrDisplayStep::onSettingsChanged,
            qontrol::UNIQUE);
    connect(m_next, &QPushButton::clicked, this, &QrDisplayStep::onNextClicked, qontrol::UNIQUE);
}

void QrDisplayStep::view() {
    m_separator = new catalog::Separator;
    m_tuning =
        (new qontrol::Row)
            ->spacing(resolve(Spacing::L))
            ->pushSpacer()
            ->pushAligned(tuneColumn(m_contrast_stepper, TR("flow-qr-contrast-hint")), Qt::AlignTop)
            ->pushAligned(tuneColumn(m_density_stepper, TR("flow-qr-density-hint")), Qt::AlignTop)
            ->pushAligned(tuneColumn(m_speed_stepper, TR("flow-qr-speed-hint")), Qt::AlignTop)
            ->pushSpacer();

    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_hint)
        ->push(m_display)
        ->push(m_separator)
        ->push(m_tuning)
        ->into(this);
}

void QrDisplayStep::setHint(const QString &hint) {
    // On one line when it fits, as the design's text takes its natural width.
    m_hint->setTextWrapped(hint, metric::QR_HINT_MAX_WIDTH);
}

void QrDisplayStep::setSize(int size) {
    m_display->setSize(size);
}

void QrDisplayStep::setTunable(bool tunable) {
    m_display->setTunable(tunable);
    m_separator->setVisible(tunable);
    m_tuning->setVisible(tunable);
    // Untuned, the code sits on plain white: the highest contrast.
    m_contrast_stepper->setValue(tunable ? qr::DEFAULT_LEVEL : qr::MAX_LEVEL);
    onSettingsChanged();
}

void QrDisplayStep::restart() {
    m_engine->restart();
}

void QrDisplayStep::stop() {
    m_engine->stop();
}

auto QrDisplayStep::engine() const -> qr::QrDisplayEngine * {
    return m_engine;
}

auto QrDisplayStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void QrDisplayStep::onSettingsChanged() {
    m_engine->setSettings(qr::QrDisplaySettings{.density = m_density_stepper->value(),
                                                .contrast = m_contrast_stepper->value(),
                                                .speed = m_speed_stepper->value()});
}

void QrDisplayStep::onFrame(const qr::QrFrame &frame) {
    bool rendering = frame.image.isNull() && frame.error.isEmpty();
    m_density_stepper->setEnabled(!rendering);
    m_next->setEnabled(!rendering);
    m_actions->setReason(rendering ? TR("flow-qr-rebuild-reason") : QString());
}

void QrDisplayStep::onNextClicked() {
    m_engine->stop();
    emit nextClicked();
}

} // namespace keys::flow
