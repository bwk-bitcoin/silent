#include "catalog/inputs/Toggle.h"
#include "theme/Palette.h"
#include "theme/Theme.h"
#include <QPainter>

namespace catalog {

namespace {
constexpr qreal DISABLED_OPACITY = 0.45;
} // namespace

Toggle::Toggle(ToggleRole role, QWidget *parent)
    : qontrol::widgets::ToggleSwitch(parent),
      m_role(role) {
    applyRole();
}

void Toggle::setRole(ToggleRole role) {
    if (m_role == role) {
        return;
    }
    m_role = role;
    applyRole();
}

auto Toggle::role() const -> ToggleRole {
    return m_role;
}

void Toggle::applyRole() {
    setProperty("class", "toggle");

    const auto &p = Theme::get()->palette();
    m_handle = p.text;

    switch (m_role) {
    case ToggleRole::Default:
        m_color_on = p.accent;
        m_color_off = p.border;
        break;
    case ToggleRole::Status:
        m_color_on = p.success;
        m_color_off = p.error;
        break;
    }

    setFixedSize(metric::TOGGLE_WIDTH, metric::TOGGLE_HEIGHT);
    update();
}

void Toggle::resizeEvent([[maybe_unused]] QResizeEvent *e) {
    // Override to prevent ToggleSwitch aspect-ratio enforcement
}

auto Toggle::hitButton(const QPoint &pos) const -> bool {
    return rect().contains(pos);
}

void Toggle::paintEvent([[maybe_unused]] QPaintEvent *e) {
    QPainter painter(this);
    painter.setPen(Qt::NoPen);
    painter.setRenderHint(QPainter::Antialiasing, true);

    int travel = width() - height();
    int outerRadius = height() / 2;
    constexpr int c_margin = 3;
    int handleRadius = (height() - (2 * c_margin)) / 2;

    // A disabled switch keeps its colors, dimmed.
    if (!isEnabled()) {
        painter.setOpacity(DISABLED_OPACITY);
    }

    // Track
    painter.setBrush(isChecked() ? m_color_on : m_color_off);
    painter.drawRoundedRect(QRect(0, 0, width(), height()), outerRadius, outerRadius);

    // Handle
    painter.setBrush(m_handle);
    auto handleRect =
        QRect(c_margin, c_margin, height() - (2 * c_margin), height() - (2 * c_margin));
    if (isChecked()) {
        handleRect.moveTo(c_margin + travel, c_margin);
    }
    painter.drawRoundedRect(handleRect, handleRadius, handleRadius);
}

auto Toggle::qss([[maybe_unused]] const Palette &p) -> QString {
    return {};
}

} // namespace catalog
