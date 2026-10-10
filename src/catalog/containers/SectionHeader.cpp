#include "catalog/containers/SectionHeader.h"

#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace catalog {

SectionHeader::SectionHeader(const QString &label, QWidget *parent) : QWidget(parent) {
    setProperty("class", "section-header");

    m_label = new Label(label, LabelRole::Eyebrow, this);
    // The fold toggle wears the same eyebrow text, led by the chevron.
    m_fold = new Button(label, ButtonRole::Menu, this);
    m_fold->setFont(m_label->font());
    m_fold->setMinimumHeight(0);
    m_fold->setIconSize(QSize(metric::FOLD_CHEVRON_SIZE, metric::FOLD_CHEVRON_SIZE));
    m_fold->setVisible(false);
    m_count = new Label(LabelRole::Mono, this);
    m_count->setSize(LabelSize::Sm);
    m_count->setTone(LabelTone::Muted);
    m_count->setVisible(false);

    m_row = (new qontrol::Row)
                ->spacing(resolve(Spacing::S))
                ->push(m_fold)
                ->push(m_label)
                ->push(m_count)
                ->pushSpacer();
    m_row->into(this);

    connect(m_fold, &QPushButton::clicked, this, &SectionHeader::onFoldClicked, qontrol::UNIQUE);
    setDense(false);
    refresh();
}

void SectionHeader::setLabel(const QString &label) {
    m_label->setText(label);
    m_fold->setText(label);
}

void SectionHeader::setCount(const QString &count) {
    m_count->setText(count);
    m_count->setVisible(!count.isEmpty());
}

void SectionHeader::setAction(QWidget *action) {
    if (m_action != nullptr) {
        m_row->remove(m_action);
    }
    m_action = action;
    if (m_action != nullptr) {
        m_row->push(m_action);
    }
    refresh();
}

void SectionHeader::setFoldable(bool foldable) {
    m_foldable = foldable;
    refresh();
}

void SectionHeader::setOpen(bool open) {
    if (m_open == open) {
        return;
    }
    m_open = open;
    refresh();
    emit toggled(m_open);
}

auto SectionHeader::isOpen() const -> bool {
    return m_open;
}

void SectionHeader::setDense(bool dense) {
    int height = dense ? resolve(BtnWidth::Icon) : metric::BUTTON_MIN_HEIGHT;
    setMinimumHeight(height);
    m_row->margins(0, 0, 0, dense ? resolve(Spacing::XXS) : resolve(Spacing::XS));
}

void SectionHeader::onFoldClicked() {
    setOpen(!m_open);
}

void SectionHeader::refresh() {
    m_fold->setVisible(m_foldable);
    m_label->setVisible(!m_foldable);
    // Chevron down when open, right when folded.
    m_fold->setIcon(m_open ? icon::chevronDown(IconColor::Secondary)
                           : icon::chevronRight(IconColor::Secondary));
    if (m_action != nullptr) {
        m_action->setVisible(!m_foldable || m_open);
    }
}

auto SectionHeader::qss(const Palette &p) -> QString {
    // The fold toggle reads as the eyebrow label, not a button.
    return QString("QWidget[class=\"section-header\"] QPushButton[role=\"menu\"], "
                   "QWidget[class=\"section-header\"] QPushButton[role=\"menu\"]:hover, "
                   "QWidget[class=\"section-header\"] QPushButton[role=\"menu\"]:pressed { "
                   "background: transparent; border: none; padding: 0; color: %1; }")
        .arg(p.textMuted.name());
}

} // namespace catalog
