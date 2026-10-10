#include "catalog/display/Badge.h"

#include "theme/Theme.h"

#include <QFontMetrics>
#include <QLabel>
#include <QStyle>
#include <Qontrol>

namespace catalog {

Badge::Badge(const QString &text, Role role, QWidget *parent) : Badge(role, parent) {
    setText(text);
}

Badge::Badge(Role role, QWidget *parent) : QFrame(parent), m_role(role) {
    setProperty("class", "badge");

    m_icon = new QLabel(this);
    m_icon->setVisible(false);
    m_text = new QLabel(this);

    m_text->setFont(textFont());

    (new qontrol::Row)
        ->margins(metric::BADGE_PADDING_H, metric::BADGE_PADDING_V, metric::BADGE_PADDING_H,
                  metric::BADGE_PADDING_V)
        ->spacing(resolve(Padding::XS))
        ->pushSpacer()
        ->push(m_icon)
        ->push(m_text)
        ->pushSpacer()
        ->into(this);

    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedHeight(pillHeight());
    applyRole();
}

void Badge::setRole(Role role) {
    if (m_role == role) {
        return;
    }
    m_role = role;
    applyRole();
    style()->unpolish(this);
    style()->polish(this);
}

auto Badge::role() const -> Role {
    return m_role;
}

void Badge::setText(const QString &text) {
    m_text->setText(text);
}

auto Badge::text() const -> QString {
    return m_text->text();
}

void Badge::setIcon(const QIcon &icon) {
    m_icon->setPixmap(icon.pixmap(metric::BADGE_ICON_SIZE, metric::BADGE_ICON_SIZE));
    m_icon->setVisible(!icon.isNull());
}

void Badge::setWidth(PillWidth width) {
    if (width == PillWidth::Auto) {
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
        return;
    }
    setFixedWidth(resolve(width));
}

auto Badge::textFont() -> QFont {
    const auto &font = Theme::get()->fontPalette().caption;
    QFont badgeFont;
    badgeFont.setFamily(font.family);
    badgeFont.setPointSize(font.size);
    badgeFont.setWeight(QFont::DemiBold);
    badgeFont.setLetterSpacing(QFont::PercentageSpacing, 102); // design 0.02em tracking
    return badgeFont;
}

auto Badge::pillHeight() -> int {
    return QFontMetrics(textFont()).height() + (2 * metric::BADGE_PADDING_V) + 2;
}

void Badge::applyRole() {
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
}

auto Badge::qss(const Palette &p) -> QString {
    // Qt paints square corners when the radius exceeds half the height, so the pill
    // has a fixed height and a radius of exactly half of it.
    return QString(
               "QFrame[class=\"badge\"] { border: 1px solid transparent; "
               "border-radius: %1px; }"
               "QFrame[class=\"badge\"][role=\"success\"] { background: %2; border-color: %3; }"
               "QFrame[class=\"badge\"][role=\"success\"] QLabel { color: %4; }"
               "QFrame[class=\"badge\"][role=\"warning\"] { background: %5; border-color: %6; }"
               "QFrame[class=\"badge\"][role=\"warning\"] QLabel { color: %7; }"
               "QFrame[class=\"badge\"][role=\"error\"] { background: %8; border-color: %9; }"
               "QFrame[class=\"badge\"][role=\"error\"] QLabel { color: %10; }"
               "QFrame[class=\"badge\"][role=\"neutral\"] { background: %11; border-color: %12; }"
               "QFrame[class=\"badge\"][role=\"neutral\"] QLabel { color: %13; }"
               "QFrame[class=\"badge\"][role=\"off\"] { background: %14; border-color: %15; }"
               "QFrame[class=\"badge\"][role=\"off\"] QLabel { color: %16; }")
        .arg(pillHeight() / 2)
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
