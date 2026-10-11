#include "catalog/containers/Modal.h"

#include "catalog/display/Label.h"

#include <QEvent>

namespace catalog {

Modal::Modal() {
    m_header = new qontrol::Row;
    m_body = new qontrol::Row;
    m_footer = new qontrol::Row;
    setMainWidget((new qontrol::Column)
                      ->margins(resolve(Padding::L))
                      ->spacing(resolve(Spacing::M))
                      ->push(m_header)
                      ->push(m_body)
                      ->push(m_footer));
}

void Modal::setHeader(QWidget *header) {
    replace(m_header, m_header_widget, header);
}

void Modal::setBody(QWidget *body) {
    replace(m_body, m_body_widget, body);
}

void Modal::setFooter(QWidget *footer) {
    replace(m_footer, m_footer_widget, footer);
}

void Modal::setWidth(ModalWidth width) {
    setFixedWidth(resolve(width));
    adjustSize();
}

auto Modal::event(QEvent *event) -> bool {
    bool handled = qontrol::Modal::event(event);
    // A size change deep in a slot reaches the window a few events after the setter
    // that caused it: fit the window again once it does.
    if (event->type() == QEvent::LayoutRequest) {
        adjustSize();
    }
    return handled;
}

void Modal::keep(QWidget *widget) {
    widget->hide();
    widget->setParent(this);
}

void Modal::replace(qontrol::Row *slot, QWidget *&current, QWidget *widget) {
    if (current != nullptr) {
        slot->take(current);
        // Spacers only: the widget was taken out first.
        slot->clear();
        // Kept alive with the modal, so the consumer can put it back later.
        keep(current);
    }
    current = widget;
    if (widget != nullptr) {
        // The body sits centred; a header or footer fills its row.
        if (slot == m_body) {
            slot->pushSpacer()->push(widget)->pushSpacer();
        } else {
            slot->push(widget);
        }
        widget->show();
    }
    slot->setVisible(widget != nullptr);
    adjustSize();
}

auto modalHeader(const QString &title, QWidget *end) -> QWidget * {
    auto *row = (new qontrol::Row)->push(new Label(title, LabelRole::Heading), 1);
    if (end != nullptr) {
        row->push(end);
    }
    return row;
}

} // namespace catalog
