#include "catalog/containers/ScrollArea.h"

#include <QScrollBar>
#include <QStyle>
#include <algorithm>
#include <common.h>

namespace catalog {

ScrollArea::ScrollArea(QWidget *parent) : QScrollArea(parent) {
    setProperty("class", "scroll-area");
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &ScrollArea::onRangeChanged,
            qontrol::UNIQUE);
}

void ScrollArea::setFitContent(bool fit) {
    m_fit = fit;
    setSizeAdjustPolicy(fit ? QAbstractScrollArea::AdjustToContents
                            : QAbstractScrollArea::AdjustIgnored);
    setSizePolicy(QSizePolicy::Expanding, fit ? QSizePolicy::Preferred : QSizePolicy::Expanding);
    updateGeometry();
}

auto ScrollArea::sizeHint() const -> QSize {
    // QScrollArea caches the first content size and caps it; the base class does not.
    if (m_fit && widget() != nullptr) {
        return QAbstractScrollArea::sizeHint();
    }
    return QScrollArea::sizeHint();
}

auto ScrollArea::minimumSizeHint() const -> QSize {
    // Never asks for more than its content, so a short list keeps its own height.
    QSize minimum = QScrollArea::minimumSizeHint();
    if (m_fit && widget() != nullptr) {
        minimum.setHeight(std::min(minimum.height(), sizeHint().height()));
    }
    return minimum;
}

auto ScrollArea::hasHeightForWidth() const -> bool {
    return m_fit && widget() != nullptr && widget()->hasHeightForWidth();
}

auto ScrollArea::heightForWidth(int width) const -> int {
    int frame = 2 * frameWidth();
    return widget()->heightForWidth(width - frame) + frame;
}

void ScrollArea::onRangeChanged(int minimum, int maximum) {
    bool overflow = maximum > minimum;
    if (property("overflow").toBool() == overflow) {
        return;
    }
    setProperty("overflow", overflow);
    style()->unpolish(this);
    style()->polish(this);
}

auto ScrollArea::qss(const Palette &p) -> QString {
    // A left margin on the vertical bar (top margin on the horizontal bar) keeps
    // a small gap between the scrolled content and the scrollbar.
    return QString(
               "QScrollArea[class=\"scroll-area\"] { border: none; border-top: 1px solid "
               "transparent; border-bottom: 1px solid transparent; background: transparent; }"
               "QScrollArea[class=\"scroll-area\"][overflow=\"true\"] { border-top-color: %2; "
               "border-bottom-color: %2; }"
               "QScrollArea[class=\"scroll-area\"] > QWidget > QWidget { background: transparent; }"
               "QScrollBar:vertical { background: transparent; width: %1px; margin: 0 0 0 %6px; }"
               "QScrollBar::handle:vertical { background: %2; border-radius: %3px; min-height: "
               "%4px; }"
               "QScrollBar::handle:vertical:hover { background: %5; }"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
               "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
               "QScrollBar:horizontal { background: transparent; height: %1px; margin: %6px 0 0 0; "
               "}"
               "QScrollBar::handle:horizontal { background: %2; border-radius: %3px; min-width: "
               "%4px; }"
               "QScrollBar::handle:horizontal:hover { background: %5; }"
               "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
               "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: "
               "none; }")
        .arg(scrollBarWidth())
        .arg(p.borderLight.name())
        .arg(radius::CHECKBOX)
        .arg(resolve(Spacing::L))
        .arg(p.border.name())
        .arg(scrollBarGap());
}

} // namespace catalog
