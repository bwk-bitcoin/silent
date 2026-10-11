#include "qr/QrCameraDisplay.h"

#include "catalog/display/Label.h"
#include "catalog/feedback/Spinner.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>

#include <Qontrol>

namespace qr {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

constexpr int ASPECT_WIDTH = 4;
constexpr int ASPECT_HEIGHT = 3;

} // namespace

QrCameraDisplay::QrCameraDisplay(QWidget *parent) : QWidget(parent) {
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    m_spinner = new catalog::Spinner(metric::CAMERA_STATE_ICON_SIZE);
    m_icon = new QLabel;
    m_title = new Label(LabelRole::Section);
    m_title->setTone(LabelTone::Inverse);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    m_body = new Label(LabelRole::Body);
    m_body->setTone(LabelTone::InverseMuted);
    m_body->setAlignment(Qt::AlignCenter);
    m_body->setWordWrap(true);

    m_overlay = (new qontrol::Column)
                    ->margins(resolve(Spacing::L))
                    ->spacing(resolve(Spacing::S))
                    ->pushSpacer()
                    ->pushAligned(m_spinner, Qt::AlignHCenter)
                    ->pushAligned(m_icon, Qt::AlignHCenter)
                    ->push(m_title)
                    ->push(m_body)
                    ->pushSpacer();
    // Laid over the picture by hand: a layout of its own would size the viewport by
    // the overlay text instead of its 4:3 ratio.
    m_overlay->setParent(this);

    setState(QrCamera::State::Starting);
}

void QrCameraDisplay::setState(QrCamera::State state) {
    using State = QrCamera::State;
    IconFn glyph = nullptr;
    QString title;
    QString body;
    switch (state) {
    case State::Starting:
        title = TR("flow-camera-starting");
        break;
    case State::None:
        glyph = icon::alert;
        title = TR("flow-camera-none-title");
        body = TR("flow-camera-none-body");
        break;
    case State::Denied:
        glyph = icon::lock;
        title = TR("flow-camera-denied-title");
        body = TR("flow-camera-denied-body");
        break;
    case State::Failed:
        glyph = icon::circleX;
        title = TR("flow-camera-failed-title");
        body = TR("flow-camera-failed-body");
        break;
    case State::Live:
        break;
    }
    if (glyph != nullptr) {
        int size = metric::CAMERA_STATE_ICON_SIZE;
        m_icon->setPixmap(glyph(IconColor::Muted).pixmap(size, size));
    }
    m_spinner->setVisible(state == State::Starting);
    m_icon->setVisible(glyph != nullptr);
    m_title->setText(title);
    m_body->setText(body);
    m_body->setVisible(!body.isEmpty());
    m_overlay->setVisible(state != State::Live);
    if (state != State::Live) {
        m_frame = QImage();
    }
    update();
}

void QrCameraDisplay::frame(const CameraFrame &frame) {
    m_frame = frame.image;
    update();
}

auto QrCameraDisplay::hasHeightForWidth() const -> bool {
    return true;
}

auto QrCameraDisplay::heightForWidth(int width) const -> int {
    return width * ASPECT_HEIGHT / ASPECT_WIDTH;
}

void QrCameraDisplay::resizeEvent(QResizeEvent *event) {
    m_overlay->setGeometry(rect());
    QWidget::resizeEvent(event);
}

void QrCameraDisplay::paintEvent([[maybe_unused]] QPaintEvent *event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath outline;
    outline.addRoundedRect(box, radius::BUTTON, radius::BUTTON);
    painter.fillPath(outline, color::CAMERA_BG);

    if (!m_frame.isNull()) {
        // Fills the box and crops the overflow, like the design's object-fit: cover.
        QSize scaled = m_frame.size().scaled(size(), Qt::KeepAspectRatioByExpanding);
        QRect target(QPoint(0, 0), scaled);
        target.moveCenter(rect().center());
        painter.save();
        painter.setClipPath(outline);
        painter.drawImage(target, m_frame);
        painter.restore();
    }

    painter.setPen(QPen(Theme::get()->palette().borderLight, 1));
    painter.drawPath(outline);
}

} // namespace qr
