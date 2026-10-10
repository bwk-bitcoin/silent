#include "catalog/feedback/ModalStatus.h"

#include "catalog/display/Label.h"
#include "catalog/feedback/Spinner.h"
#include "theme/Palette.h"

#include <QLabel>
#include <Qontrol>

namespace catalog {

ModalStatus::ModalStatus(QWidget *parent) : QWidget(parent) {
    m_spinner = new Spinner(metric::SPINNER_SIZE, this);
    m_spinner->setVisible(false);
    m_icon = new QLabel(this);
    m_icon->setAlignment(Qt::AlignCenter);
    m_icon->setVisible(false);
    m_title = new Label(LabelRole::Section, this);
    auto titleFont = m_title->font();
    titleFont.setWeight(QFont::Medium); // design title is weight 500
    m_title->setFont(titleFont);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    m_title->setVisible(false);
    m_subtitle = new Label(LabelRole::Body, this);
    m_subtitle->setTone(LabelTone::Muted);
    m_subtitle->setAlignment(Qt::AlignCenter);
    m_subtitle->setVisible(false);

    m_column = (new qontrol::Column)
                   ->spacing(resolve(Spacing::M))
                   ->pushAligned(m_spinner, Qt::AlignHCenter)
                   ->pushAligned(m_icon, Qt::AlignHCenter)
                   ->pushAligned(m_title, Qt::AlignHCenter)
                   ->pushAligned(m_subtitle, Qt::AlignHCenter);
    m_column->into(this);
    setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

void ModalStatus::setSpinner() {
    m_icon->setVisible(false);
    m_spinner->setVisible(true);
}

void ModalStatus::setIcon(IconFn icon, IconColor color) {
    int size = metric::MODAL_STATUS_ICON_SIZE;
    m_icon->setPixmap(icon(color).pixmap(size, size));
    m_spinner->setVisible(false);
    m_icon->setVisible(true);
}

void ModalStatus::setTitle(const QString &title) {
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
}

void ModalStatus::setSubtitle(const QString &subtitle) {
    m_subtitle->setTextWrapped(subtitle, metric::MODAL_STATUS_TEXT_MAX_WIDTH);
    m_subtitle->setVisible(!subtitle.isEmpty());
}

void ModalStatus::setPadding(int top, int bottom) {
    m_column->margins(0, top, 0, bottom);
}

} // namespace catalog
