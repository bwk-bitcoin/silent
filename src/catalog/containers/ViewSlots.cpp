#include "catalog/containers/ViewSlots.h"

#include "theme/Palette.h"

#include <QDialog>
#include <Qontrol>

namespace catalog {

ViewSlots::ViewSlots(QWidget *parent) : QWidget(parent) {
    m_header = new qontrol::Row;
    m_body = new qontrol::Row;
    m_footer = new qontrol::Row;
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_header)
        // A window taller than its content (a wizard step) gives the room to the body.
        ->push(m_body, 1)
        ->push(m_footer)
        ->into(this);
}

void ViewSlots::setHeader(QWidget *header) {
    replace(m_header, m_header_widget, header, false);
}

void ViewSlots::setBody(QWidget *body) {
    replace(m_body, m_body_widget, body, true);
}

void ViewSlots::setWideBody(QWidget *body) {
    replace(m_body, m_body_widget, body, false);
}

void ViewSlots::setFooter(QWidget *footer) {
    replace(m_footer, m_footer_widget, footer, false);
}

void ViewSlots::setPage(QWidget *page) {
    setHeader(nullptr);
    setFooter(nullptr);
    replace(m_body, m_body_widget, page, false);
}

void ViewSlots::keep(QWidget *widget) {
    widget->hide();
    widget->setParent(this);
}

void ViewSlots::replace(qontrol::Row *slot, QWidget *&current, QWidget *widget, bool centred) {
    if (current != nullptr) {
        slot->take(current);
        // Spacers only: the widget was taken out first.
        slot->clear();
        // Kept alive here, so the consumer can put it back later.
        keep(current);
    }
    current = widget;
    if (widget != nullptr) {
        if (centred) {
            slot->pushSpacer()->push(widget)->pushSpacer();
        } else {
            slot->push(widget);
        }
        widget->show();
    }
    slot->setVisible(widget != nullptr);
    // A modal fits its new view; a page of the main window (the Settings tab) leaves
    // the window its size.
    if (qobject_cast<QDialog *>(window()) != nullptr) {
        window()->adjustSize();
    }
}

} // namespace catalog
