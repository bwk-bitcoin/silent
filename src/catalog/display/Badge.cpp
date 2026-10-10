#include "catalog/display/Badge.h"

#include "theme/Theme.h"

namespace catalog {

Badge::Badge(const QString &text, Role role, QWidget *parent) : QLabel(text, parent), m_role(role) {
    applyRole();
}

Badge::Badge(Role role, QWidget *parent) : QLabel(parent), m_role(role) {
    applyRole();
}

void Badge::setRole(Role role) {
    if (m_role == role) {
        return;
    }
    m_role = role;
    applyRole();
}

auto Badge::role() const -> Role {
    return m_role;
}

void Badge::applyRole() {
    setProperty("class", "badge");
    setAlignment(Qt::AlignCenter);

    switch (m_role) {
    case Role::Success:
        setProperty("role", "success");
        break;
    case Role::Warning:
        setProperty("role", "warning");
        break;
    case Role::Error:
        setProperty("role", "error");
        break;
    case Role::Neutral:
        setProperty("role", "neutral");
        break;
    case Role::Off:
        setProperty("role", "off");
        break;
    }

    const auto &font = Theme::get()->fontPalette().caption;
    auto badgeFont = this->font();
    badgeFont.setFamily(font.family);
    badgeFont.setPointSize(font.size);
    badgeFont.setWeight(QFont::DemiBold);
    badgeFont.setLetterSpacing(QFont::PercentageSpacing, 102); // design 0.02em tracking
    setFont(badgeFont);
}

auto Badge::qss(const Palette &p) -> QString {
    return QString(
               "QLabel[class=\"badge\"] { border: 1px solid transparent; "
               "border-radius: %1px; padding: %2px %3px; }"
               "QLabel[class=\"badge\"][role=\"success\"] { background: %4; border-color: %5; "
               "color: %6; }"
               "QLabel[class=\"badge\"][role=\"warning\"] { background: %7; border-color: %8; "
               "color: %9; }"
               "QLabel[class=\"badge\"][role=\"error\"] { background: %10; border-color: %11; "
               "color: %12; }"
               "QLabel[class=\"badge\"][role=\"neutral\"] { background: %13; border-color: %14; "
               "color: %15; }"
               "QLabel[class=\"badge\"][role=\"off\"] { background: %16; border-color: %17; "
               "color: %18; }")
        .arg(radius::BADGE)
        .arg(resolve(Padding::XS) - 1)
        .arg(resolve(Padding::S) + 1)
        .arg(qssColor(p.badgeSuccessBg))
        .arg(qssColor(p.badgeSuccessBorder))
        .arg(p.badgeSuccessText.name())
        .arg(qssColor(p.badgeWarningBg))
        .arg(qssColor(p.badgeWarningBorder))
        .arg(p.badgeWarningText.name())
        .arg(qssColor(p.badgeErrorBg))
        .arg(qssColor(p.badgeErrorBorder))
        .arg(p.badgeErrorText.name())
        .arg(p.badgeNeutralBg.name())
        .arg(p.badgeNeutralBorder.name())
        .arg(p.badgeNeutralText.name())
        .arg(qssColor(p.badgeOffBg))
        .arg(p.badgeOffBorder.name())
        .arg(p.badgeOffText.name());
}

} // namespace catalog
