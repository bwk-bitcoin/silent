#include "catalog/containers/ScrollBar.h"

#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionSlider>

namespace catalog {

ScrollBar::ScrollBar(Qt::Orientation orientation, QWidget *parent)
    : QScrollBar(orientation, parent) {
    setMouseTracking(true);
}

void ScrollBar::paintEvent(QPaintEvent * /*event*/) {
    if (!isEnabled() || maximum() <= minimum()) {
        return;
    }
    const auto &palette = Theme::get()->palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_hovered || isSliderDown() ? palette.border : palette.borderLight);
    painter.drawRoundedRect(handleRect(), radius::CHECKBOX, radius::CHECKBOX);
}

void ScrollBar::mouseMoveEvent(QMouseEvent *event) {
    bool hovered = handleRect().contains(event->position().toPoint());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
    QScrollBar::mouseMoveEvent(event);
}

void ScrollBar::leaveEvent(QEvent *event) {
    if (m_hovered) {
        m_hovered = false;
        update();
    }
    QScrollBar::leaveEvent(event);
}

auto ScrollBar::handleRect() const -> QRect {
    QStyleOptionSlider option;
    initStyleOption(&option);
    return style()->subControlRect(QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, this);
}

} // namespace catalog
