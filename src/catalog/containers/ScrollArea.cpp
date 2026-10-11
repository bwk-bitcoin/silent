#include "catalog/containers/ScrollArea.h"

#include "theme/Theme.h"

#include <QEvent>
#include <QPainter>
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
    // Always there, so the right gutter is the bar's whether the content overflows or
    // not; with nothing to scroll it is disabled and draws nothing.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    verticalScrollBar()->setEnabled(false);
    applyMargins();
}

void ScrollArea::setFitContent(bool fit) {
    m_fit = fit;
    setSizeAdjustPolicy(fit ? QAbstractScrollArea::AdjustToContents
                            : QAbstractScrollArea::AdjustIgnored);
    setSizePolicy(QSizePolicy::Expanding, fit ? QSizePolicy::Preferred : QSizePolicy::Expanding);
    updateGeometry();
}

auto ScrollArea::sizeHint() const -> QSize {
    if (m_fit && widget() != nullptr) {
        // The gutters are reserved whether the scrollbar shows or not, so the hint
        // does not change with it.
        int frame = 2 * frameWidth();
        QSize content = contentSize();
        return {content.width() + (2 * insetH()) + frame,
                content.height() + (2 * insetV()) + frame};
    }
    return QScrollArea::sizeHint();
}

auto ScrollArea::minimumSizeHint() const -> QSize {
    // Never asks for more than its content, so a short list keeps its own height.
    QSize minimum = QScrollArea::minimumSizeHint();
    if (m_fit && widget() != nullptr) {
        minimum.setHeight(std::min(minimum.height(), sizeHint().height()));
        // A width set on the content holds.
        minimum.setWidth(std::max(minimum.width(),
                                  widget()->minimumWidth() + (2 * insetH()) + (2 * frameWidth())));
    }
    return minimum;
}

auto ScrollArea::hasHeightForWidth() const -> bool {
    return m_fit && widget() != nullptr && widget()->hasHeightForWidth();
}

auto ScrollArea::heightForWidth(int width) const -> int {
    int frame = 2 * frameWidth();
    return widget()->heightForWidth(width - frame - (2 * insetH())) + frame + (2 * insetV());
}

auto ScrollArea::event(QEvent *event) -> bool {
    bool handled = QScrollArea::event(event);
    // The lines are drawn across the whole area, gutters included, so they reach past
    // the content on both sides and read as the scroll edges.
    if (event->type() == QEvent::Paint && m_overflow) {
        QPainter painter(this);
        QColor line = Theme::get()->palette().borderLight;
        painter.fillRect(QRect(0, 0, width(), EDGE_LINE), line);
        painter.fillRect(QRect(0, height() - EDGE_LINE, width(), EDGE_LINE), line);
    }
    // A turned off bar leaves its gutter to the margin.
    if (event->type() == QEvent::LayoutRequest) {
        applyMargins();
    }
    return handled;
}

auto ScrollArea::viewportEvent(QEvent *event) -> bool {
    // The content's size change stops at the viewport, which has no layout: pass it
    // on, so a fitting area asks its parent for the new height.
    if (m_fit && event->type() == QEvent::LayoutRequest) {
        updateGeometry();
    }
    return QScrollArea::viewportEvent(event);
}

auto ScrollArea::eventFilter(QObject *watched, QEvent *event) -> bool {
    // The content relaying itself out (rows added, shown, polished) changes the
    // height a fitting area asks for, but nothing tells the layout holding the area,
    // which keeps the old hint: drop it.
    if (m_fit && watched == widget() && event->type() == QEvent::LayoutRequest) {
        updateGeometry();
    }
    return QScrollArea::eventFilter(watched, event);
}

void ScrollArea::applyMargins() {
    int right = barShown() ? resolve(Spacing::XS) : insetH();
    QMargins margins(insetH(), insetV(), right, insetV());
    if (viewportMargins() != margins) {
        setViewportMargins(margins);
    }
}

auto ScrollArea::barShown() const -> bool {
    return verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn;
}

auto ScrollArea::contentSize() const -> QSize {
    QSize size = widget()->sizeHint();
    size.setWidth(std::clamp(size.width(), widget()->minimumWidth(), widget()->maximumWidth()));
    return size;
}

void ScrollArea::onRangeChanged(int minimum, int maximum) {
    bool overflow = maximum > minimum;
    if (overflow == m_overflow) {
        return;
    }
    m_overflow = overflow;
    verticalScrollBar()->setEnabled(overflow);
    update();
}

auto ScrollArea::qss(const Palette &p) -> QString {
    // No background on the scroll area itself: a transparent one becomes its palette,
    // and the style draws the scrollbar from it as a dark track. Only the viewport
    // (and the content in it) is made transparent, to show what lies behind.
    // The bar is the design's: a thumb its whole width on a transparent track, the
    // gap to the content being the area's padding.
    return QString(
               "QScrollArea[class=\"scroll-area\"] { border: none; }"
               "QScrollArea[class=\"scroll-area\"] > QWidget#qt_scrollarea_viewport { background: "
               "transparent; }"
               "QScrollArea[class=\"scroll-area\"] > QWidget > QWidget { background: transparent; }"
               "QScrollBar:vertical { background: transparent; width: %1px; margin: 0; }"
               "QScrollBar::handle:vertical { background: %2; border-radius: %3px; min-height: "
               "%4px; }"
               "QScrollBar::handle:vertical:disabled { background: transparent; }"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
               "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
               "QScrollBar:horizontal { background: transparent; height: %1px; margin: 0; }"
               "QScrollBar::handle:horizontal { background: %2; border-radius: %3px; min-width: "
               "%4px; }"
               "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
               "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: "
               "none; }")
        .arg(scrollBarWidth())
        .arg(p.border.name())
        .arg(radius::SCROLLBAR)
        .arg(resolve(Spacing::L));
}

} // namespace catalog
