#include "catalog/containers/Tab.h"
#include "theme/Palette.h"

namespace catalog {

auto Tab::qss(const Palette &p) -> QString {
    // Space above and before the tabs, as the strip pads them.
    return QString(
               "QTabWidget::pane { border: 1px solid %1; }"
               "QTabWidget::tab-bar { left: %7px; }"
               "QTabBar::tab { background: %2; color: %3; padding: %7px %8px; margin-top: %7px; "
               "  font-size: %11pt; "
               "  border: 1px solid %1; border-bottom: none; border-top-left-radius: %9px; "
               "  border-top-right-radius: %9px; margin-right: %10px; }"
               "QTabBar::tab:selected { background: %4; color: %5; }"
               "QTabBar::tab:hover:!selected { background: %6; }"
               // An idle module bar's current tab is not the page shown: drawn unselected.
               "QTabBar[class=\"module-tabs\"][idle=\"true\"]::tab:selected { background: %2; "
               "  color: %3; }"
               "QTabBar[class=\"module-tabs\"][idle=\"true\"]::tab:selected:hover { "
               "  background: %6; }"
               // The settings gear's slot after the tabs.
               "QFrame[class=\"tabstrip-end\"] { border: none; border-left: 1px solid %12; }")
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
        .arg(size::BODY)             // %11
        .arg(p.borderLight.name());  // %12
}

} // namespace catalog
