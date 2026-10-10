#include "catalog/Button.h"
#include "theme/Palette.h"
#include "theme/Theme.h"
#include <QFont>
#include <QStyle>
#include <QTimer>
#include <common.h>

namespace catalog {

Button::Button(const QString &text, ButtonRole role, QWidget *parent)
    : QPushButton(text, parent),
      m_role(role) {
    applyRole();
}

Button::Button(ButtonRole role, QWidget *parent) : QPushButton(parent), m_role(role) {
    applyRole();
}

void Button::setRole(ButtonRole role) {
    if (m_role == role) {
        return;
    }
    m_role = role;
    applyRole();
    style()->unpolish(this);
    style()->polish(this);
}

auto Button::role() const -> ButtonRole {
    return m_role;
}

void Button::setWidth(BtnWidth width) {
    if (width == BtnWidth::Auto) {
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
        return;
    }
    setFixedWidth(resolve(width));
}

void Button::setActive(bool active) {
    setProperty("active", active);
    style()->unpolish(this);
    style()->polish(this);
}

void Button::setFeedbackIcon(const QIcon &icon, const QString &tooltip, int duration_ms) {
    m_feedback_icon = icon;
    m_feedback_tooltip = tooltip;
    // what to revert to after the feedback window
    m_base_icon = this->icon();
    m_base_tooltip = toolTip();
    if (m_feedback_timer == nullptr) {
        m_feedback_timer = new QTimer(this);
        m_feedback_timer->setSingleShot(true);
        connect(m_feedback_timer, &QTimer::timeout, this, &Button::onFeedbackRestore,
                qontrol::UNIQUE);
        connect(this, &QPushButton::clicked, this, &Button::onFeedbackClicked, qontrol::UNIQUE);
    }
    m_feedback_timer->setInterval(duration_ms);
}

void Button::onFeedbackClicked() {
    setIcon(m_feedback_icon);
    if (!m_feedback_tooltip.isEmpty()) {
        setToolTip(m_feedback_tooltip);
    }
    m_feedback_timer->start();
}

void Button::onFeedbackRestore() {
    setIcon(m_base_icon);
    if (!m_feedback_tooltip.isEmpty()) {
        setToolTip(m_base_tooltip);
    }
}

static auto styleForRole(const ButtonPalette &bp, ButtonRole role) -> const ButtonStyle & {
    switch (role) {
    case ButtonRole::Primary:
        return bp.primary;
    case ButtonRole::Destructive:
        return bp.destructive;
    case ButtonRole::Menu:
        return bp.menu;
    case ButtonRole::Icon:
        return bp.icon;
    case ButtonRole::InlineIcon:
        return bp.inlineIcon;
    case ButtonRole::TabOpen:
        return bp.tabOpen;
    case ButtonRole::TabCreate:
        return bp.tabCreate;
    case ButtonRole::Inline:
        return bp.inlineBtn;
    case ButtonRole::Index:
        return bp.index;
    case ButtonRole::Default:
        return bp.defaultBtn;
    }
    return bp.defaultBtn;
}

void Button::applyRole() {
    setProperty("class", "button");

    switch (m_role) {
    case ButtonRole::Primary:
        setProperty("role", "primary");
        break;
    case ButtonRole::Destructive:
        setProperty("role", "destructive");
        break;
    case ButtonRole::Menu:
        setProperty("role", "menu");
        break;
    case ButtonRole::Icon:
        setProperty("role", "icon");
        break;
    case ButtonRole::InlineIcon:
        setProperty("role", "inline-icon");
        break;
    case ButtonRole::TabOpen:
        setProperty("role", "tab-open");
        break;
    case ButtonRole::TabCreate:
        setProperty("role", "tab-create");
        break;
    case ButtonRole::Inline:
        setProperty("role", "inline");
        break;
    case ButtonRole::Index:
        setProperty("role", "index");
        break;
    case ButtonRole::Default:
        setProperty("role", "default");
        break;
    }

    const auto &bp = Theme::get()->buttonPalette();
    const auto &s = styleForRole(bp, m_role);

    auto f = font();
    f.setPointSize(s.font.size);
    f.setFamily(s.font.family);
    f.setWeight(s.font.weight);
    setFont(f);

    if (s.width != width::NONE) {
        setFixedWidth(s.width);
    } else {
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
    }

    if (s.height != width::NONE) {
        setFixedHeight(s.height);
    } else {
        setMinimumHeight(metric::BUTTON_MIN_HEIGHT);
        setMaximumHeight(QWIDGETSIZE_MAX);
    }
}

auto Button::qss(const Palette &p) -> QString {
    const auto &bp = Theme::get()->buttonPalette();
    const auto &defaultBtn = bp.defaultBtn;
    const auto &primary = bp.primary;
    const auto &destructive = bp.destructive;
    const auto &menu = bp.menu;
    const auto &icon = bp.icon;
    const auto &inlineIcon = bp.inlineIcon;

    // --- Base ---
    auto baseQss =
        QString("QPushButton[class=\"button\"] { padding: %1px %2px; border-radius: %3px; "
                "  border: 1px solid %4; background: %5; color: %6; }"
                "QPushButton[class=\"button\"]:hover { background: %7; }"
                "QPushButton[class=\"button\"]:pressed { background: %8; }"
                "QPushButton[class=\"button\"]:disabled { color: %9; border-color: %10; }")
            .arg(defaultBtn.paddingV)
            .arg(defaultBtn.paddingH)
            .arg(defaultBtn.borderRadius)
            .arg(p.border.name())
            .arg(p.bgSecondary.name())
            .arg(p.text.name())
            .arg(p.surfaceHover.name())
            .arg(p.bg.name())
            .arg(p.textMuted.name())
            .arg(p.borderLight.name());

    // --- Primary ---
    auto primaryQss =
        QString("QPushButton[class=\"button\"][role=\"primary\"] { background: %1; color: %2; "
                "  border: none; padding: %3px %4px; border-radius: %5px; }"
                "QPushButton[class=\"button\"][role=\"primary\"]:hover { background: %6; }"
                "QPushButton[class=\"button\"][role=\"primary\"]:pressed { background: %1; }"
                "QPushButton[class=\"button\"][role=\"primary\"]:disabled { background: %7; color: "
                "%8; }")
            .arg(p.accent.name())
            .arg(p.onAccent.name())
            .arg(primary.paddingV)
            .arg(primary.paddingH)
            .arg(primary.borderRadius)
            .arg(p.accentHover.name())
            .arg(p.borderLight.name())
            .arg(p.textMuted.name());

    // --- Destructive ---
    auto destructiveQss =
        QString(
            "QPushButton[class=\"button\"][role=\"destructive\"] { background: %1; color: %2; "
            "  border: none; padding: %3px %4px; border-radius: %5px; }"
            "QPushButton[class=\"button\"][role=\"destructive\"]:hover { background: %6; }"
            "QPushButton[class=\"button\"][role=\"destructive\"]:pressed { background: %1; }"
            "QPushButton[class=\"button\"][role=\"destructive\"]:disabled { background: %7; color: "
            "%8; }")
            .arg(p.error.name())
            .arg(p.onAccent.name())
            .arg(destructive.paddingV)
            .arg(destructive.paddingH)
            .arg(destructive.borderRadius)
            .arg(p.errorHover.name())
            .arg(p.borderLight.name())
            .arg(p.textMuted.name());

    // --- Menu ---
    auto menuQss =
        QString("QPushButton[class=\"button\"][role=\"menu\"] { background: %1; border: none; "
                "  border-radius: %2px; padding: %3px %4px; color: %5; text-align: left; }"
                "QPushButton[class=\"button\"][role=\"menu\"]:hover { background: %6; }"
                "QPushButton[class=\"button\"][role=\"menu\"]:pressed { background: %7; }"
                "QPushButton[class=\"button\"][role=\"menu\"]:checked { background: %7; }")
            .arg(p.bgSecondary.name())
            .arg(menu.borderRadius)
            .arg(menu.paddingV)
            .arg(menu.paddingH)
            .arg(p.text.name())
            .arg(p.surfaceHover.name())
            .arg(p.bg.name());

    // --- Inline ---
    const auto &inl = bp.inlineBtn;
    auto inlineQss =
        QString("QPushButton[class=\"button\"][role=\"inline\"] { background: %1; "
                "  border: 1px solid %2; border-radius: %3px; padding: %4px %5px; color: %6; }"
                "QPushButton[class=\"button\"][role=\"inline\"]:hover { background: %7; }"
                "QPushButton[class=\"button\"][role=\"inline\"]:pressed { background: %8; }"
                "QPushButton[class=\"button\"][role=\"inline\"]:disabled { color: %9; "
                "  border-color: %10; }")
            .arg(p.bgSecondary.name())  // %1
            .arg(p.inputBorder.name())  // %2
            .arg(inl.borderRadius)      // %3
            .arg(inl.paddingV)          // %4
            .arg(inl.paddingH)          // %5
            .arg(p.text.name())         // %6
            .arg(p.surfaceHover.name()) // %7
            .arg(p.bg.name())           // %8
            .arg(p.textMuted.name())    // %9
            .arg(p.borderLight.name()); // %10

    // Size comes solely from setFixedSize() in applyRole(). Any width/height/
    // min-height here fights it: width/height (a QSS content box) overflows the
    // fixed widget and clips the border, and min-height:0 lets the row squash it.
    auto iconQss =
        QString("QPushButton[class=\"button\"][role=\"icon\"] { padding: 0; border-radius: %1px; }"
                "QPushButton[class=\"button\"][role=\"inline-icon\"] { padding: 0; "
                "border-radius: %2px; }")
            .arg(icon.borderRadius)
            .arg(inlineIcon.borderRadius);

    // --- Index (QR glyph + derivation index handle) ---
    const auto &index = bp.index;
    auto indexQss = QString("QPushButton[class=\"button\"][role=\"index\"] { padding: %1px %2px; "
                            "border-radius: %3px; }"
                            "QPushButton[class=\"button\"][role=\"index\"][active=\"true\"] { "
                            "background: %4; }")
                        .arg(index.paddingV)
                        .arg(index.paddingH)
                        .arg(index.borderRadius)
                        .arg(p.bg.name());

    // Overlay icon button: fully transparent (no background or border in any
    // state); the icon color is changed by the call site on hover/press.
    auto overlayQss = QString(
        "QPushButton[class=\"button\"][overlay=\"true\"] { background: transparent; border: none; }"
        "QPushButton[class=\"button\"][overlay=\"true\"]:hover { background: transparent; }"
        "QPushButton[class=\"button\"][overlay=\"true\"]:pressed { background: transparent; }");

    return baseQss + primaryQss + destructiveQss + menuQss + inlineQss + iconQss + indexQss +
           overlayQss;
}

} // namespace catalog
