#include "catalog/inputs/DateTimeInput.h"

#include "theme/Theme.h"

namespace catalog {

DateTimeInput::DateTimeInput(QWidget *parent) : QDateTimeEdit(parent) {
    setProperty("class", "datetime-input");
    setCalendarPopup(true);
    setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    const auto &mono = Theme::get()->inputPalette().mono.font;
    auto f = font();
    f.setFamily(mono.family);
    f.setPointSize(mono.size);
    setFont(f);
}

void DateTimeInput::setWidth(Size s) {
    setFixedWidth(resolve(s));
}

auto DateTimeInput::qss(const Palette &p) -> QString {
    return QString(
               "QDateTimeEdit[class=\"datetime-input\"] {"
               "  border: 1px solid %1;"
               "  background: %2;"
               "  border-radius: %3px;"
               "  padding: %4px %5px;"
               "  color: %6;"
               "}"
               "QDateTimeEdit[class=\"datetime-input\"]:focus { border: 1px solid %7; }"
               "QDateTimeEdit[class=\"datetime-input\"]:disabled {"
               "  background: %8; color: %9; border-color: %10;"
               "}"
               "QDateTimeEdit[class=\"datetime-input\"]::drop-down { border: none; width: %11px; }")
        .arg(p.inputBorder.name())          // %1
        .arg(p.inputBg.name())              // %2
        .arg(radius::INPUT)                 // %3
        .arg(resolve(Padding::XS))          // %4
        .arg(resolve(Padding::S))           // %5
        .arg(p.text.name())                 // %6
        .arg(p.inputFocus.name())           // %7
        .arg(p.bgSecondary.name())          // %8
        .arg(p.textSecondary.name())        // %9
        .arg(p.borderLight.name())          // %10
        .arg(metric::COMBO_DROPDOWN_WIDTH); // %11
}

} // namespace catalog
