#include "catalog/containers/Modal.h"

#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"

#include <QApplication>
#include <QEvent>
#include <QLayout>

namespace catalog {

Modal::Modal() {
    m_slots = new ViewSlots;
    setMainWidget((new qontrol::Column)->margins(resolve(Padding::L))->push(m_slots));
}

void Modal::setHeader(QWidget *header) {
    m_slots->setHeader(header);
}

void Modal::setBody(QWidget *body) {
    m_slots->setBody(body);
}

void Modal::setFooter(QWidget *footer) {
    m_slots->setFooter(footer);
}

void Modal::setPage(QWidget *page) {
    m_slots->setPage(page);
}

void Modal::setVisible(bool visible) {
    if (visible && !isVisible()) {
        // Without a parent the window manager places the modal where it likes: give it
        // the window it opens over, so it is centred there.
        QWidget *over = QApplication::activeWindow();
        if (parentWidget() == nullptr && over != nullptr && over != this) {
            setParent(over, windowFlags());
        }
        // The position is computed from the size at this point: lay the content out
        // first, or the modal is centred at its unlaid size and grows down from there.
        layout()->activate();
        adjustSize();
    }
    qontrol::Modal::setVisible(visible);
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
    m_slots->keep(widget);
}

auto modalHeader(const QString &title, QWidget *end) -> QWidget * {
    auto *row = (new qontrol::Row)->push(new Label(title, LabelRole::Heading), 1);
    if (end != nullptr) {
        row->push(end);
    }
    return row;
}

} // namespace catalog
