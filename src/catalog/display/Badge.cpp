#include "catalog/display/Badge.h"

#include "theme/Theme.h"

#include <QFontMetrics>
#include <QLabel>
#include <QStyle>
#include <Qontrol>

namespace catalog {

namespace {

// components.css .s-tbl__cell .s-badge: 11px over 12px, padding 3px 8px, gap 4px and
// the pill widths times 0.85.
constexpr qreal DENSE_FONT_SCALE = 11.0 / 12.0;
constexpr int DENSE_PADDING_H = 8;
constexpr int DENSE_PADDING_V = 3;
constexpr int DENSE_GAP = 4;
constexpr qreal DENSE_WIDTH_SCALE = 0.85;

} // namespace

Badge::Badge(const QString &text, Role role, QWidget *parent) : Badge(role, parent) {
    setText(text);
}

Badge::Badge(Role role, QWidget *parent) : QFrame(parent), m_role(role) {
    setProperty("class", "badge");

    m_icon = new QLabel(this);
    m_icon->setVisible(false);
    m_text = new QLabel(this);

    m_row = (new qontrol::Row)->pushSpacer()->push(m_icon)->push(m_text)->pushSpacer();
    m_row->into(this);

    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    applyMetrics();
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
    m_width = width;
    applyMetrics();
}

void Badge::setDense(bool dense) {
    if (m_dense == dense) {
        return;
    }
    m_dense = dense;
    setProperty("dense", dense);
    applyMetrics();
    style()->unpolish(this);
    style()->polish(this);
}

void Badge::applyMetrics() {
    int padH = m_dense ? DENSE_PADDING_H : metric::BADGE_PADDING_H;
    int padV = m_dense ? DENSE_PADDING_V : metric::BADGE_PADDING_V;
    m_row->margins(padH, padV, padH, padV)->spacing(m_dense ? DENSE_GAP : resolve(Padding::XS));
    m_text->setFont(textFont(m_dense));
    setFixedHeight(pillHeight(m_dense));

    if (m_width == PillWidth::Auto) {
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
        return;
    }
    qreal scale = m_dense ? DENSE_WIDTH_SCALE : 1.0;
    setFixedWidth(qRound(resolve(m_width) * scale));
}

auto Badge::textFont(bool dense) -> QFont {
    const auto &font = Theme::get()->fontPalette().caption;
    QFont badgeFont;
    badgeFont.setFamily(font.family);
    badgeFont.setPointSizeF(dense ? font.size * DENSE_FONT_SCALE : font.size);
    badgeFont.setWeight(font::DEMIBOLD);
    badgeFont.setLetterSpacing(QFont::PercentageSpacing, 102); // design 0.02em tracking
    return badgeFont;
}

auto Badge::pillHeight(bool dense) -> int {
    int padV = dense ? DENSE_PADDING_V : metric::BADGE_PADDING_V;
    return QFontMetrics(textFont(dense)).height() + (2 * padV) + 2;
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
               "QFrame[class=\"badge\"][role=\"off\"] QLabel { color: %16; }"
               "QFrame[class=\"badge\"][dense=\"true\"] { border-radius: %17px; }")
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
        .arg(p.badgeOffText.name())
        .arg(pillHeight(true) / 2);
}

} // namespace catalog
