#include "QrSettings.h"

#include "catalog/display/Label.h"
#include "catalog/inputs/Stepper.h"
#include "i18n/Tr.h"
#include "qr/QrDisplay.h"
#include "qr/QrDisplayEngine.h"
#include "qr/QrTypes.h"
#include "theme/Palette.h"

#include <silent.h>

#include <Qontrol>
#include <common.h>

#include <cstdint>

namespace keys::flow {

namespace {

// One tuning control: the stepper over the line that says what it does.
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

auto levelStepper(const QString &label, int value) -> catalog::Stepper * {
    auto *stepper = new catalog::Stepper;
    stepper->setLabel(label);
    stepper->setRange(qr::MIN_LEVEL, qr::MAX_LEVEL);
    stepper->setValue(value);
    return stepper;
}

} // namespace

void applyQrLevels(qr::QrDisplayEngine *engine) {
    auto levels = ::app_qr_display_levels();
    engine->setDensity(qr::densityFromLevel(levels.density));
    engine->setContrast(qr::contrastFromLevel(levels.contrast));
    engine->setSpeed(qr::speedFromLevel(levels.speed));
}

QrSettings::QrSettings(qr::QrDisplayEngine *engine) : m_engine(engine) {
    setWindowTitle(TR("flow-qr-settings-title"));
    init();
    doConnect();
    view();
}

void QrSettings::init() {
    auto levels = ::app_qr_display_levels();
    m_contrast = levelStepper(TR("keys-qr-contrast"), levels.contrast);
    m_density = levelStepper(TR("keys-qr-density"), levels.density);
    m_speed = levelStepper(TR("flow-qr-speed"), levels.speed);
    m_display = new qr::QrDisplay;
    m_display->setSize(metric::QR_REQUEST_SIZE);
}

void QrSettings::doConnect() {
    for (auto *stepper : {m_contrast, m_density, m_speed}) {
        connect(stepper, &catalog::Stepper::valueChanged, this, &QrSettings::onLevelChanged,
                qontrol::UNIQUE);
    }
    connect(m_engine, &qr::QrDisplayEngine::frame, this, &QrSettings::onFrame, qontrol::UNIQUE);
    connect(m_engine, &qr::QrDisplayEngine::frame, m_display, &qr::QrDisplay::frame,
            qontrol::UNIQUE);
}

void QrSettings::view() {
    auto *controls =
        (new qontrol::Row)
            ->spacing(resolve(Spacing::L))
            ->pushAligned(tuneColumn(m_contrast, TR("flow-qr-contrast-hint")), Qt::AlignTop)
            ->pushAligned(tuneColumn(m_density, TR("flow-qr-density-hint")), Qt::AlignTop)
            ->pushAligned(tuneColumn(m_speed, TR("flow-qr-speed-hint")), Qt::AlignTop);
    // The code being tuned, so each change shows here rather than behind the modal.
    setBody((new qontrol::Column)
                ->spacing(resolve(Spacing::L))
                ->pushAligned(m_display, Qt::AlignHCenter)
                ->pushAligned(controls, Qt::AlignHCenter));
    setWidth(ModalWidth::M);
    m_engine->refresh();
}

void QrSettings::onLevelChanged() {
    ::app_set_qr_display_levels(QrDisplayLevels{
        .density = static_cast<std::uint8_t>(m_density->value()),
        .contrast = static_cast<std::uint8_t>(m_contrast->value()),
        .speed = static_cast<std::uint8_t>(m_speed->value()),
    });
    applyQrLevels(m_engine);
}

void QrSettings::onFrame(const qr::QrFrame &frame) {
    // A density change rebuilds every part: no other one until it is done.
    bool rendering = frame.image.isNull() && frame.error.isEmpty();
    m_density->setEnabled(!rendering);
}

} // namespace keys::flow
