#include "catalog/containers/Tab.h"
#include "theme/Palette.h"

namespace catalog {

auto Tab::qss(const Palette &p) -> QString {
    return QString("QTabWidget::pane { border: 1px solid %1; }"
                   "QTabBar::tab { background: %2; color: %3; padding: %7px %8px; "
                   "  font-size: %11pt; "
                   "  border: 1px solid %1; border-bottom: none; border-top-left-radius: %9px; "
                   "  border-top-right-radius: %9px; margin-right: %10px; }"
                   "QTabBar::tab:selected { background: %4; color: %5; }"
                   "QTabBar::tab:hover:!selected { background: %6; }")
        .arg(p.border.name())        // %1
        .arg(p.bgSecondary.name())   // %2
        .arg(p.textSecondary.name()) // %3
        .arg(p.bg.name())            // %4
        .arg(p.text.name())          // %5
        .arg(p.surfaceHover.name())  // %6
        .arg(resolve(Padding::XS))   // %7
        .arg(resolve(Padding::M))    // %8
        .arg(radius::TAB)            // %9
        .arg(resolve(Spacing::XXS))  // %10
        .arg(size::BODY);            // %11
}

} // namespace catalog
