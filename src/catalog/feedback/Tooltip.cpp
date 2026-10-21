#include "catalog/feedback/Tooltip.h"

#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QEvent>
#include <QWidget>

namespace catalog {

namespace {

// Widgets.jsx Tooltip: 500 11px, padding 5px 9px, 8px above its anchor.
constexpr int TIP_FONT_PX = 11;
constexpr int TIP_PAD_H = 9;
constexpr int TIP_PAD_V = 5;
constexpr int TIP_GAP = 8;

} // namespace

// A tooltip window of its own, so no container (a table cell, a scroll area) clips it.
Tooltip::Tooltip(const QString &text, QWidget *parent) : QLabel(text, parent) {
    setProperty("class", "tooltip");
    setWindowFlags(Qt::ToolTip);
    setContentsMargins(TIP_PAD_H, TIP_PAD_V, TIP_PAD_H, TIP_PAD_V);

    const auto &fp = Theme::get()->fontPalette().caption;
    auto f = font();
    f.setFamily(fp.family);
    f.setPixelSize(TIP_FONT_PX);
    f.setWeight(QFont::Medium);
    setFont(f);

    hide();
}

void Tooltip::attachTo(QWidget *widget) {
    widget->installEventFilter(this);
}

auto Tooltip::eventFilter(QObject *watched, QEvent *event) -> bool {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (widget == nullptr) {
        return QLabel::eventFilter(watched, event);
    }

    if (event->type() == QEvent::Enter) {
        // Centered above the widget.
        adjustSize();
        QPoint anchor = widget->mapToGlobal(QPoint(widget->width() / 2, 0));
        move(anchor.x() - (width() / 2), anchor.y() - TIP_GAP - height());
        show();
    }
    if (event->type() == QEvent::Leave) {
        hide();
    }

    return QLabel::eventFilter(watched, event);
}

auto Tooltip::qss(const Palette &p) -> QString {
    return QString("QLabel[class=\"tooltip\"] { background: %1; color: %2; border-radius: %3px; }")
        .arg(p.text.name())
        .arg(p.bg.name())
        .arg(radius::ICON);
}

} // namespace catalog
