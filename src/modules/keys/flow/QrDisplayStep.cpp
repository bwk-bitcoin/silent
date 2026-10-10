#include "QrDisplayStep.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Spinner.h"
#include "catalog/inputs/Stepper.h"
#include "catalog/panels/receive/QrCode.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QGridLayout>
#include <QImage>
#include <QTimer>

#include <cstdint>
#include <utility>

#include <Qontrol>
#include <common.h>

namespace keys::flow {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;
using catalog::Stepper;

namespace {

// Contrast sets the quiet zone's lightness, on the design's blue grey: the lowest
// level is a dark grey, the highest white.
constexpr int BACKGROUND_HUE = 220;
constexpr qreal BACKGROUND_SATURATION = 0.14;
constexpr int MIN_LIGHTNESS_PERCENT = 40;
constexpr int LIGHTNESS_STEP_PERCENT = 15;
constexpr qreal PERCENT = 100.0;
constexpr qreal FULL_CIRCLE = 360.0;
constexpr int MAX_COLOR_VALUE = 255;

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

} // namespace

QrDisplayStep::QrDisplayStep(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
    setRebuilding();
}

void QrDisplayStep::init() {
    m_hint = new Label(LabelRole::Body);
    m_hint->setTone(LabelTone::Secondary);
    m_hint->setWordWrap(true);
    m_hint->setAlignment(Qt::AlignCenter);

    m_qr_code = new catalog::QrCode;
    m_qr_code->setFixedSize(metric::QR_DISPLAY_SIZE, metric::QR_DISPLAY_SIZE);
    m_qr_code->setPadding(metric::QR_DISPLAY_PADDING);
    m_qr_code->setRadius(radius::QR_CARD);

    auto *rebuildingText = new Label(TR("flow-qr-rebuilding"), LabelRole::Body);
    auto rebuildingFont = rebuildingText->font();
    rebuildingFont.setWeight(QFont::DemiBold);
    rebuildingText->setFont(rebuildingFont);
    m_rebuilding =
        (new qontrol::Column)
            ->spacing(resolve(Spacing::S))
            ->pushAligned(new catalog::Spinner(metric::QR_REBUILD_SPINNER_SIZE), Qt::AlignHCenter)
            ->pushAligned(rebuildingText, Qt::AlignHCenter);

    m_part_label = new Label(LabelRole::Mono);
    m_part_label->setTone(LabelTone::Secondary);
    auto partFont = m_part_label->font();
    partFont.setWeight(QFont::DemiBold);
    m_part_label->setFont(partFont);
    m_part_label->setAlignment(Qt::AlignCenter);

    m_contrast_stepper = new Stepper;
    m_contrast_stepper->setLabel(TR("keys-qr-contrast"));
    m_contrast_stepper->setRange(MIN_LEVEL, MAX_LEVEL);
    m_contrast_stepper->setValue(DEFAULT_LEVEL);
    m_density_stepper = new Stepper;
    m_density_stepper->setLabel(TR("keys-qr-density"));
    m_density_stepper->setRange(MIN_LEVEL, MAX_LEVEL);
    m_density_stepper->setValue(DEFAULT_LEVEL);
    m_speed_stepper = new Stepper;
    m_speed_stepper->setLabel(TR("flow-qr-speed"));
    m_speed_stepper->setRange(MIN_LEVEL, MAX_LEVEL);
    m_speed_stepper->setValue(DEFAULT_LEVEL);

    m_next = catalog::btnNextScanTheAnswer();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_next);

    m_frame_timer = new QTimer(this);
    m_frame_timer->setInterval(frameInterval());

    rebuildColorTable();
}

void QrDisplayStep::doConnect() {
    connect(m_frame_timer, &QTimer::timeout, this, &QrDisplayStep::onFrameTick, qontrol::UNIQUE);
    connect(m_contrast_stepper, &Stepper::valueChanged, this, &QrDisplayStep::onContrastChanged,
            qontrol::UNIQUE);
    connect(m_density_stepper, &Stepper::valueChanged, this, &QrDisplayStep::onDensityChanged,
            qontrol::UNIQUE);
    connect(m_speed_stepper, &Stepper::valueChanged, this, &QrDisplayStep::onSpeedChanged,
            qontrol::UNIQUE);
    connect(m_next, &QPushButton::clicked, this, &QrDisplayStep::onNextClicked, qontrol::UNIQUE);
}

void QrDisplayStep::view() {
    // The rebuilding overlay sits on top of the code, in the same grid cell.
    auto *codeCell = new QWidget;
    auto *grid = new QGridLayout(codeCell);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->addWidget(m_qr_code, 0, 0, Qt::AlignCenter);
    grid->addWidget(m_rebuilding, 0, 0, Qt::AlignCenter);

    auto *tuning =
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
        ->pushAligned(codeCell, Qt::AlignHCenter)
        ->push(m_part_label)
        ->push(new catalog::Separator)
        ->push(tuning)
        ->into(this);
}

void QrDisplayStep::setHint(const QString &hint) {
    m_hint->setText(hint);
}

void QrDisplayStep::setRequest(rust::Box<QrRequest> request) {
    m_request = std::make_optional(std::move(request));
    m_frame_index = 0;
    setRebuildingShown(false);
    if (!m_request.value()->is_ok()) {
        m_part_label->setText(
            TR("keys-qr-frame-failed")
                .arg(QString::fromStdString(std::string(m_request.value()->get_error()))));
        return;
    }
    if (showFrame(m_frame_index)) {
        m_frame_timer->start();
    }
}

void QrDisplayStep::setRebuilding() {
    m_frame_timer->stop();
    setRebuildingShown(true);
}

void QrDisplayStep::restart() {
    m_frame_index = 0;
    if (showFrame(m_frame_index)) {
        m_frame_timer->start();
    }
}

void QrDisplayStep::stop() {
    m_frame_timer->stop();
}

auto QrDisplayStep::density() const -> int {
    return m_density;
}

auto QrDisplayStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void QrDisplayStep::setRebuildingShown(bool rebuilding) {
    m_qr_code->setDimmed(rebuilding);
    m_rebuilding->setVisible(rebuilding);
    m_density_stepper->setEnabled(!rebuilding);
    m_next->setEnabled(!rebuilding);
    m_actions->setReason(rebuilding ? TR("flow-qr-rebuild-reason") : QString());
    if (rebuilding) {
        m_part_label->setText(TR("flow-qr-part-pending"));
    }
}

auto QrDisplayStep::showFrame(int index) -> bool {
    if (!m_request.has_value() || !m_request.value()->is_ok()) {
        return false;
    }
    auto result = m_request.value()->frame(static_cast<std::uint32_t>(index));
    if (!result.is_ok) {
        m_frame_timer->stop();
        m_part_label->setText(
            TR("keys-qr-frame-failed").arg(QString::fromStdString(std::string(result.error))));
        return false;
    }
    QImage image(result.data.data(), static_cast<int>(result.width),
                 static_cast<int>(result.height), static_cast<int>(result.width),
                 QImage::Format_Grayscale8);
    m_qr_code->setImage(applyContrast(image));
    m_part_label->setText(
        TR("keys-qr-part").arg(m_frame_index + 1).arg(m_request.value()->frame_count()));
    return true;
}

auto QrDisplayStep::frameInterval() const -> int {
    return BASE_INTERVAL_MS - m_speed * SPEED_STEP_MS;
}

auto QrDisplayStep::background() const -> QColor {
    int lightness = MIN_LIGHTNESS_PERCENT + (m_contrast - MIN_LEVEL) * LIGHTNESS_STEP_PERCENT;
    return QColor::fromHslF(BACKGROUND_HUE / FULL_CIRCLE, BACKGROUND_SATURATION,
                            lightness / PERCENT);
}

auto QrDisplayStep::applyContrast(const QImage &frame) const -> QImage {
    QImage indexed(frame.constBits(), frame.width(), frame.height(), frame.bytesPerLine(),
                   QImage::Format_Indexed8);
    indexed.setColorTable(m_color_table);
    return indexed.copy();
}

void QrDisplayStep::rebuildColorTable() {
    QColor quietZone = background();
    m_color_table.resize(MAX_COLOR_VALUE + 1);
    for (int level = 0; level <= MAX_COLOR_VALUE; ++level) {
        m_color_table[level] =
            mixColor(Qt::black, quietZone, static_cast<qreal>(level) / MAX_COLOR_VALUE).rgb();
    }
    m_qr_code->setBackground(quietZone);
}

void QrDisplayStep::onFrameTick() {
    if (!m_request.has_value() || m_request.value()->frame_count() == 0) {
        return;
    }
    m_frame_index = (m_frame_index + 1) % static_cast<int>(m_request.value()->frame_count());
    showFrame(m_frame_index);
}

void QrDisplayStep::onContrastChanged(int value) {
    m_contrast = value;
    rebuildColorTable();
    showFrame(m_frame_index);
}

void QrDisplayStep::onDensityChanged(int value) {
    m_density = value;
    setRebuilding();
    emit densityChanged(m_density);
}

void QrDisplayStep::onSpeedChanged(int value) {
    m_speed = value;
    m_frame_timer->setInterval(frameInterval());
}

void QrDisplayStep::onNextClicked() {
    m_frame_timer->stop();
    emit nextClicked();
}

} // namespace keys::flow
