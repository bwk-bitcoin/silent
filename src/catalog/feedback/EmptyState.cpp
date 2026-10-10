#include "catalog/feedback/EmptyState.h"

#include "catalog/display/Label.h"

#include <QLabel>
#include <Qontrol>

namespace catalog {

EmptyState::EmptyState(IconFn icon, const QString &title, const QString &subtitle, QWidget *parent)
    : Card(Role::Inset, parent) {

    auto *glyph = new QLabel;
    glyph->setPixmap(icon(IconColor::Muted)
                         .pixmap(metric::EMPTY_STATE_ICON_SIZE, metric::EMPTY_STATE_ICON_SIZE));

    m_title = new Label(title, LabelRole::Section);
    auto titleFont = m_title->font();
    titleFont.setWeight(QFont::Medium); // design title is weight 500
    m_title->setFont(titleFont);
    m_title->setAlignment(Qt::AlignCenter);

    m_subtitle = new Label(LabelRole::Body);
    m_subtitle->setTone(LabelTone::Muted);
    m_subtitle->setAlignment(Qt::AlignCenter);
    m_subtitle->setTextWrapped(subtitle, metric::MODAL_STATUS_TEXT_MAX_WIDTH);
    m_subtitle->setVisible(!subtitle.isEmpty());

    m_column = (new qontrol::Column)
                   ->margins(0, resolve(Spacing::S), 0, resolve(Spacing::S))
                   ->spacing(resolve(Spacing::M))
                   ->pushAligned(glyph, Qt::AlignHCenter)
                   ->pushAligned(m_title, Qt::AlignHCenter)
                   ->pushAligned(m_subtitle, Qt::AlignHCenter);
    setContent(m_column);
}

void EmptyState::setTitle(const QString &title) {
    m_title->setText(title);
}

void EmptyState::setSubtitle(const QString &subtitle) {
    m_subtitle->setTextWrapped(subtitle, metric::MODAL_STATUS_TEXT_MAX_WIDTH);
    m_subtitle->setVisible(!subtitle.isEmpty());
}

void EmptyState::setAction(QWidget *action) {
    if (m_action != nullptr) {
        m_column->remove(m_action);
    }
    m_action = action;
    if (m_action != nullptr) {
        m_column->pushAligned(m_action, Qt::AlignHCenter);
    }
}

} // namespace catalog
