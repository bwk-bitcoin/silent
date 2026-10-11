#include "catalog/SelectRow.h"

#include "catalog/display/Label.h"

#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <Qontrol>

namespace catalog {

namespace {

constexpr qreal SELECTED_TINT = 0.10;
constexpr qreal SELECTED_HOVER_TINT = 0.14;
constexpr qreal ERROR_TINT = 0.08;
constexpr qreal ERROR_BORDER_ALPHA = 0.45;

} // namespace

SelectRow::SelectRow(QWidget *parent) : QFrame(parent) {
    setProperty("class", "select-row");
    setAttribute(Qt::WA_Hover, true);

    m_icon = new QLabel(this);
    m_icon->setVisible(false);

    m_title = new Label(LabelRole::Body, this);
    auto titleFont = m_title->font();
    titleFont.setWeight(font::DEMIBOLD);
    m_title->setFont(titleFont);
    m_sub = new Label(LabelRole::Caption, this);
    m_sub->setWordWrap(true);
    m_sub->setPrefersOneLine(true);
    m_sub->setVisible(false);
    auto *body = (new qontrol::Column)->spacing(resolve(Spacing::XXS))->push(m_title)->push(m_sub);

    m_meta = new Label(LabelRole::Mono, this);
    m_meta->setSize(LabelSize::Sm);
    m_meta->setTone(LabelTone::Muted);
    m_meta->setVisible(false);

    m_status_slot = new qontrol::Row;
    m_status_slot->setVisible(false);

    m_chevron = new QLabel(this);
    m_chevron->setPixmap(
        icon::chevronRight(IconColor::Muted).pixmap(metric::CHEVRON_SIZE, metric::CHEVRON_SIZE));
    m_trailing_slot = (new qontrol::Row)->pushSpacer()->push(m_chevron);

    m_check = new QLabel(this);
    m_check->setVisible(false);

    (new qontrol::Row)
        ->margins(metric::SELECT_ROW_PADDING_H, metric::SELECT_ROW_PADDING_V,
                  metric::SELECT_ROW_PADDING_H, metric::SELECT_ROW_PADDING_V)
        ->spacing(resolve(Spacing::M))
        ->push(m_icon)
        ->push(body, 1)
        ->push(m_meta)
        ->push(m_status_slot)
        ->push(m_trailing_slot)
        ->push(m_check)
        ->into(this);

    refresh();
}

void SelectRow::setIcon(IconFn icon, int size) {
    m_icon_fn = icon;
    m_icon_size = size;
    m_icon->setVisible(m_icon_fn != nullptr);
    refreshIcon();
}

void SelectRow::setTitle(const QString &title) {
    m_title->setText(title);
}

void SelectRow::setSub(const QString &sub) {
    m_sub->setText(sub);
    m_sub->setVisible(!sub.isEmpty());
}

void SelectRow::setMeta(const QString &meta) {
    m_meta->setText(meta);
    m_meta->setVisible(m_meta_fixed || !meta.isEmpty());
}

void SelectRow::setMetaWidth(int width) {
    m_meta_fixed = true;
    m_meta->setFixedWidth(width);
    m_meta->setVisible(true);
}

void SelectRow::setStatus(QWidget *status) {
    if (m_status != nullptr) {
        m_status_slot->remove(m_status);
    }
    m_status = status;
    if (m_status != nullptr) {
        m_status_slot->push(m_status);
    }
    m_status_slot->setVisible(m_status_fixed || m_status != nullptr);
}

void SelectRow::setStatusWidth(int width) {
    m_status_fixed = true;
    m_status_slot->setFixedWidth(width);
    m_status_slot->setVisible(true);
}

void SelectRow::setTrailing(QWidget *trailing) {
    if (m_trailing != nullptr) {
        m_trailing_slot->remove(m_trailing);
    }
    m_trailing = trailing;
    if (m_trailing != nullptr) {
        m_trailing_slot->insert(1, m_trailing);
    }
    refresh();
}

void SelectRow::setTrailingWidth(int width) {
    m_trailing_fixed = true;
    m_trailing_slot->setFixedWidth(width);
    refresh();
}

void SelectRow::setChevron(bool shown) {
    m_chevron_set = true;
    m_chevron_shown = shown;
    refresh();
}

void SelectRow::setClickable(bool clickable) {
    m_clickable = clickable;
    refresh();
}

void SelectRow::setSelected(bool selected) {
    m_selectable = true;
    m_selected = selected;
    refresh();
}

void SelectRow::setCurrent(bool current) {
    m_current = current;
    refresh();
}

void SelectRow::setTone(Tone tone) {
    m_tone = tone;
    m_sub->setTone(m_tone == Tone::Error ? LabelTone::Error : LabelTone::Muted);
    refresh();
}

auto SelectRow::isActivatable() const -> bool {
    return m_clickable && isEnabled();
}

auto SelectRow::showsChevron() const -> bool {
    if (m_chevron_set) {
        return m_chevron_shown;
    }
    return isActivatable() && m_trailing == nullptr && !m_selectable;
}

void SelectRow::refresh() {
    bool activatable = isActivatable();
    setProperty("selected", m_selected || m_current);
    setProperty("disabled", !isEnabled());
    setProperty("static", !m_clickable && isEnabled() && !m_selected && !m_current);
    setProperty("tone", m_tone == Tone::Error ? "error" : "default");
    setFocusPolicy(activatable ? Qt::StrongFocus : Qt::NoFocus);
    setCursor(activatable ? Qt::PointingHandCursor : Qt::ArrowCursor);

    m_chevron->setVisible(showsChevron());
    m_trailing_slot->setVisible(m_trailing_fixed || m_trailing != nullptr || showsChevron());

    m_check->setVisible(m_selected);
    if (m_selected) {
        m_check->setPixmap(icon::circleCheck(IconColor::Accent).pixmap(m_icon_size, m_icon_size));
    }

    refreshIcon();
    repolish();
}

void SelectRow::refreshIcon() {
    if (m_icon_fn == nullptr) {
        return;
    }
    IconColor color = IconColor::Accent;
    if (!isEnabled()) {
        color = IconColor::Muted;
    } else if (m_tone == Tone::Error) {
        color = IconColor::Error;
    }
    m_icon->setPixmap(m_icon_fn(color).pixmap(m_icon_size, m_icon_size));
}

void SelectRow::repolish() {
    style()->unpolish(this);
    style()->polish(this);
    // Descendants match on this row's properties (disabled text, off badges).
    for (auto *child : findChildren<QWidget *>()) {
        style()->unpolish(child);
        style()->polish(child);
    }
}

void SelectRow::mouseReleaseEvent(QMouseEvent *event) {
    bool onTrailing = m_trailing != nullptr && m_trailing_slot->isVisible() &&
                      m_trailing_slot->geometry().contains(event->position().toPoint());
    if (event->button() == Qt::LeftButton && isActivatable() && !onTrailing &&
        rect().contains(event->position().toPoint())) {
        emit clicked();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void SelectRow::keyPressEvent(QKeyEvent *event) {
    bool activates = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
                     event->key() == Qt::Key_Space;
    if (activates && isActivatable()) {
        emit clicked();
        return;
    }
    QFrame::keyPressEvent(event);
}

void SelectRow::changeEvent(QEvent *event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) {
        refresh();
    }
}

auto SelectRow::qss(const Palette &p) -> QString {
    auto row = QString("QFrame[class=\"select-row\"]");
    auto base = QString("%1 { background: %2; border: 1px solid %3; border-radius: %4px; }"
                        "%1[tone=\"error\"] { background: %5; border-color: %6; }"
                        "%1:hover { background: %7; border-color: %8; }"
                        "%1:focus { border-color: %9; }")
                    .arg(row)
                    .arg(p.surface.name())
                    .arg(p.borderLight.name())
                    .arg(radius::BUTTON)
                    .arg(mixColor(p.surface, p.error, ERROR_TINT).name())
                    .arg(qssColor(withAlpha(p.error, ERROR_BORDER_ALPHA)))
                    .arg(p.surfaceHover.name())
                    .arg(p.border.name())
                    .arg(p.inputFocus.name());

    auto states =
        QString("%1[selected=\"true\"] { background: %2; border-color: %3; }"
                "%1[selected=\"true\"]:hover { background: %4; }"
                "%1[static=\"true\"], %1[static=\"true\"]:hover { background: %5; "
                "border-color: %6; }"
                "%1[static=\"true\"][tone=\"error\"], %1[static=\"true\"][tone=\"error\"]:hover { "
                "background: %7; border-color: %8; }"
                "%1[disabled=\"true\"], %1[disabled=\"true\"]:hover { background: %9; "
                "border-color: %6; }")
            .arg(row)
            .arg(mixColor(p.surface, p.accent, SELECTED_TINT).name())
            .arg(p.accent.name())
            .arg(mixColor(p.surface, p.accent, SELECTED_HOVER_TINT).name())
            .arg(p.surface.name())
            .arg(p.borderLight.name())
            .arg(mixColor(p.surface, p.error, ERROR_TINT).name())
            .arg(qssColor(withAlpha(p.error, ERROR_BORDER_ALPHA)))
            .arg(p.bgSecondary.name());

    // Inside a disabled row the text goes muted and every pill reads as off.
    auto disabled = QString("%1[disabled=\"true\"] QLabel { color: %2; }"
                            "%1[disabled=\"true\"] QFrame[class=\"badge\"] { background: %3; "
                            "border-color: %4; }"
                            "%1[disabled=\"true\"] QFrame[class=\"badge\"] QLabel { color: %5; }")
                        .arg(row)
                        .arg(p.textMuted.name())
                        .arg(qssColor(p.badgeOffBg))
                        .arg(p.badgeOffBorder.name())
                        .arg(p.badgeOffText.name());

    return base + states + disabled;
}

} // namespace catalog
