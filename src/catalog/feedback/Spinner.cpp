#include "catalog/feedback/Spinner.h"

#include "theme/Icon.h"

#include <QPainter>
#include <QVariantAnimation>
#include <common.h>

namespace catalog {

namespace {

constexpr int TURN_MS = 1000;
constexpr qreal FULL_TURN = 360.0;

} // namespace

Spinner::Spinner(int size, QWidget *parent) : QWidget(parent), m_size(size) {
    setFixedSize(size, size);
    m_animation = new QVariantAnimation(this);
    m_animation->setStartValue(0.0);
    m_animation->setEndValue(FULL_TURN);
    m_animation->setDuration(TURN_MS);
    m_animation->setLoopCount(-1);
    connect(m_animation, &QVariantAnimation::valueChanged, this, &Spinner::onAngleChanged,
            qontrol::UNIQUE);
}

void Spinner::onAngleChanged(const QVariant &angle) {
    m_angle = angle.toReal();
    update();
}

void Spinner::paintEvent([[maybe_unused]] QPaintEvent *event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.rotate(m_angle);
    painter.translate(-m_size / 2.0, -m_size / 2.0);
    icon::refresh(IconColor::Accent).paint(&painter, QRect(0, 0, m_size, m_size));
}

void Spinner::showEvent(QShowEvent *event) {
    m_animation->start();
    QWidget::showEvent(event);
}

void Spinner::hideEvent(QHideEvent *event) {
    m_animation->stop();
    QWidget::hideEvent(event);
}

} // namespace catalog
