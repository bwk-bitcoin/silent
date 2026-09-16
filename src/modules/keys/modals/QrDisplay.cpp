#include "QrDisplay.h"

#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Stepper.h"
#include "catalog/panels/receive/QrCode.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <QImage>
#include <QList>
#include <QRgb>
#include <QTimer>

#include <cstdint>
#include <utility>

#include <common.h>

namespace keys::modal {

using catalog::Button;
using catalog::ButtonRole;
using catalog::Label;
using catalog::LabelRole;
using catalog::QrCode;
using catalog::Stepper;

namespace {

// Contrast maps to the quiet zone's background lightness: MIN_LEVEL is a dark grey,
// MAX_LEVEL is white.
constexpr int MIN_BACKGROUND_LIGHTNESS_PERCENT = 40;
constexpr int LIGHTNESS_STEP_PERCENT = 15;
constexpr int MAX_LIGHTNESS_PERCENT = 100;
constexpr int MAX_COLOR_VALUE = 255;

} // namespace

QrDisplay::QrDisplay(const QString &title, rust::Box<QrRequest> request, QWidget *parent)
    : m_title(title),
      m_request(std::make_optional(std::move(request))) {
    Q_UNUSED(parent);
    setWindowTitle(m_title);
    init();
    doConnect();
    view();
    if (m_request.has_value() && m_request.value()->is_ok()) {
        showFrame(m_frame_index);
        m_frame_timer->start();
    }
}

void QrDisplay::init() {
    m_qr_code = new QrCode;
    m_part_label = new Label(QString(), LabelRole::Mono);

    m_contrast_stepper = new Stepper;
    m_contrast_stepper->setRange(MIN_LEVEL, MAX_LEVEL);
    m_contrast_stepper->setValue(DEFAULT_LEVEL);

    m_density_stepper = new Stepper;
    m_density_stepper->setRange(MIN_LEVEL, MAX_LEVEL);
    m_density_stepper->setValue(DEFAULT_LEVEL);

    m_speed_stepper = new Stepper;
    m_speed_stepper->setRange(MIN_LEVEL, MAX_LEVEL);
    m_speed_stepper->setValue(DEFAULT_LEVEL);

    m_next_btn = new Button(TR("keys-qr-next"), ButtonRole::Primary);
    m_next_btn->setIcon(icon::chevronRight());

    m_cancel_btn = new Button(TR("common-cancel"));

    m_frame_timer = new QTimer(this);
    m_frame_timer->setInterval(frameInterval());

    rebuildColorTable();
}

void QrDisplay::doConnect() {
    connect(m_frame_timer, &QTimer::timeout, this, &QrDisplay::onFrameTick, qontrol::UNIQUE);
    connect(m_contrast_stepper, &Stepper::valueChanged, this, &QrDisplay::onContrastChanged,
            qontrol::UNIQUE);
    connect(m_density_stepper, &Stepper::valueChanged, this, &QrDisplay::onDensityChanged,
            qontrol::UNIQUE);
    connect(m_speed_stepper, &Stepper::valueChanged, this, &QrDisplay::onSpeedChanged,
            qontrol::UNIQUE);
    connect(m_next_btn, &QPushButton::clicked, this, &QrDisplay::onNextClicked, qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &QrDisplay::onCancelClicked,
            qontrol::UNIQUE);
}

void QrDisplay::view() {
    bool ok = m_request.has_value() && m_request.value()->is_ok();

    QString errorText;
    if (!ok && m_request.has_value()) {
        errorText = QString::fromStdString(std::string(m_request.value()->get_error().c_str()));
    }
    auto *errorLabel = new Label(errorText, LabelRole::Body);
    errorLabel->setWordWrap(true);
    errorLabel->setVisible(!ok);

    auto *contrastCol = (new qontrol::Column)
                            ->push(new Label(TR("keys-qr-contrast"), LabelRole::Caption))
                            ->push(m_contrast_stepper);
    auto *densityCol = (new qontrol::Column)
                           ->push(new Label(TR("keys-qr-density"), LabelRole::Caption))
                           ->push(m_density_stepper);
    auto *speedCol = (new qontrol::Column)
                         ->push(new Label(TR("keys-qr-speed"), LabelRole::Caption))
                         ->push(m_speed_stepper);

    auto *controlsRow = (new qontrol::Row)
                            ->push(contrastCol)
                            ->pushSpacer(resolve(Spacing::M))
                            ->push(densityCol)
                            ->pushSpacer(resolve(Spacing::M))
                            ->push(speedCol)
                            ->pushSpacer()
                            ->push(m_cancel_btn)
                            ->pushSpacer(resolve(Spacing::S))
                            ->push(m_next_btn);

    auto *qrRow = (new qontrol::Row)->pushSpacer()->push(m_qr_code)->pushSpacer();
    auto *partRow = (new qontrol::Row)->pushSpacer()->push(m_part_label)->pushSpacer();

    qrRow->setVisible(ok);
    partRow->setVisible(ok);
    controlsRow->setVisible(ok);

    auto *col = (new qontrol::Column)
                    ->push(errorLabel)
                    ->push(qrRow)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(partRow)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(controlsRow);

    setMainWidget(margin(col));
}

auto QrDisplay::showFrame(int index) -> bool {
    if (!m_request.has_value()) {
        return false;
    }

    auto result = m_request.value()->frame(static_cast<std::uint32_t>(index));
    if (!result.is_ok) {
        m_frame_timer->stop();
        m_part_label->setText(TR("keys-qr-frame-failed")
                                  .arg(QString::fromStdString(std::string(result.error.c_str()))));
        return false;
    }

    QImage image(result.data.data(), static_cast<int>(result.width),
                 static_cast<int>(result.height), static_cast<int>(result.width),
                 QImage::Format_Grayscale8);
    m_qr_code->setImage(applyContrast(image));
    updatePartLabel();
    return true;
}

void QrDisplay::updatePartLabel() {
    if (!m_request.has_value()) {
        return;
    }
    m_part_label->setText(
        TR("keys-qr-part").arg(m_frame_index + 1).arg(m_request.value()->frame_count()));
}

auto QrDisplay::frameInterval() const -> int {
    return BASE_INTERVAL_MS - m_speed * SPEED_STEP_MS;
}

auto QrDisplay::applyContrast(const QImage &frame) const -> QImage {
    QImage indexed(frame.constBits(), frame.width(), frame.height(), frame.bytesPerLine(),
                   QImage::Format_Indexed8);
    indexed.setColorTable(m_color_table);
    return indexed.copy();
}

void QrDisplay::rebuildColorTable() {
    int lightnessPercent =
        MIN_BACKGROUND_LIGHTNESS_PERCENT + (m_contrast - MIN_LEVEL) * LIGHTNESS_STEP_PERCENT;
    lightnessPercent = qMin(lightnessPercent, MAX_LIGHTNESS_PERCENT);
    int backgroundLevel = MAX_COLOR_VALUE * lightnessPercent / MAX_LIGHTNESS_PERCENT;

    m_color_table.resize(MAX_COLOR_VALUE + 1);
    for (int i = 0; i <= MAX_COLOR_VALUE; ++i) {
        int level = backgroundLevel * i / MAX_COLOR_VALUE;
        m_color_table[i] = qRgb(level, level, level);
    }
}

void QrDisplay::onFrameTick() {
    if (!m_request.has_value()) {
        return;
    }
    m_frame_index = (m_frame_index + 1) % static_cast<int>(m_request.value()->frame_count());
    showFrame(m_frame_index);
}

void QrDisplay::onContrastChanged(int value) {
    m_contrast = value;
    rebuildColorTable();
    showFrame(m_frame_index);
}

void QrDisplay::onDensityChanged(int value) {
    m_density = value;
    m_frame_timer->stop();
    m_density_stepper->setEnabled(false);
    m_part_label->setText(TR("keys-qr-rebuilding"));
    emit densityChanged(m_density);
}

void QrDisplay::onSpeedChanged(int value) {
    m_speed = value;
    m_frame_timer->setInterval(frameInterval());
}

void QrDisplay::onNextClicked() {
    m_frame_timer->stop();
    emit nextRequested();
    accept();
}

void QrDisplay::onCancelClicked() {
    m_frame_timer->stop();
    reject();
}

void QrDisplay::setRequest(rust::Box<QrRequest> request) {
    m_request = std::make_optional(std::move(request));
    m_frame_index = 0;
    m_density_stepper->setEnabled(true);
    if (showFrame(m_frame_index)) {
        m_frame_timer->start();
    }
}

void QrDisplay::restartAnimation() {
    m_frame_index = 0;
    if (showFrame(m_frame_index)) {
        m_frame_timer->start();
    }
}

auto QrDisplay::density() const -> int {
    return m_density;
}

} // namespace keys::modal
