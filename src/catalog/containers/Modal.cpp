#include "catalog/containers/Modal.h"

#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"

#include <QApplication>
#include <QEvent>
#include <QLayout>
#include <QLoggingCategory>
#include <QScreen>
#include <QStyle>
#include <algorithm>

namespace catalog {

namespace {

// Enable with QT_LOGGING_RULES="silent.modal.debug=true".
Q_LOGGING_CATEGORY(lcModal, "silent.modal", QtWarningMsg)

// The window a modal opens over: the active one, else the application's main window,
// as the active window can be unset when a click does not give the window focus.
auto openOver(const QWidget *modal) -> QWidget * {
    QWidget *active = QApplication::activeWindow();
    if (active != nullptr && active != modal) {
        return active;
    }
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (widget != modal && widget->isVisible() && widget->windowType() == Qt::Window) {
            return widget;
        }
    }
    return nullptr;
}

} // namespace

Modal::Modal() {
    closeOnlyTitlebar(this);
    m_slots = new ViewSlots;
    setMainWidget((new qontrol::Column)->margins(resolve(Padding::L))->push(m_slots));
}

void Modal::setHeader(QWidget *header) {
    m_slots->setHeader(header);
}

void Modal::setBody(QWidget *body) {
    m_slots->setBody(body);
}

void Modal::setWideBody(QWidget *body) {
    m_slots->setWideBody(body);
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
        QWidget *over = openOver(this);
        qCDebug(lcModal) << "opening" << this << "parent" << parentWidget() << "active"
                         << QApplication::activeWindow() << "over" << over;
        if (parentWidget() == nullptr && over != nullptr) {
            setParent(over, windowFlags());
        }
        // The position is computed from the size at this point: lay the content out
        // first, or the modal is centred at its unlaid size and grows down from there.
        activateLayouts();
        refit();
    }
    qontrol::Modal::setVisible(visible);
    if (visible) {
        qCDebug(lcModal) << "opened" << this << "at" << geometry() << "over"
                         << (parentWidget() != nullptr ? parentWidget()->window()->geometry()
                                                       : QRect());
    }
}

void Modal::setWidth(ModalWidth width) {
    setFixedWidth(resolve(width));
    refit();
}

void Modal::setWizardHeight(bool wizard) {
    m_wizard_height = wizard;
    refit();
}

void Modal::activateLayouts() {
    // Reversed, every widget comes before its parent: each one hands its new size up
    // before the parent lays out.
    auto widgets = findChildren<QWidget *>();
    for (auto it = widgets.crbegin(); it != widgets.crend(); ++it) {
        if ((*it)->layout() != nullptr) {
            (*it)->layout()->activate();
        }
    }
    layout()->activate();
}

void Modal::refit() {
    // Free the height pinned by the last refit, so the content sets it again.
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);
    adjustSize();
    int height = this->height();
    // adjustSize() sizes wrapped text for the content's preferred width, narrower than
    // the modal's fixed one, so it leaves the height of lines that no longer wrap.
    if (layout()->hasHeightForWidth()) {
        height = layout()->totalHeightForWidth(width());
    }
    if (m_wizard_height) {
        height = std::max(height, metric::MODAL_WIZARD_HEIGHT);
    }
    // No taller than the screen: the window manager would shrink it back, the resize
    // would refit it, and the two would fight. Capped, a fitting body scrolls instead.
    if (QScreen *on = screen(); on != nullptr) {
        int titlebar = frameGeometry().height() - this->height();
        if (titlebar <= 0) {
            titlebar = style()->pixelMetric(QStyle::PM_TitleBarHeight);
        }
        height = std::min(height, on->availableGeometry().height() - titlebar);
    }
    // Pinned: a window that cannot be resized gets no maximize button from KWin, which
    // ignores the titlebar hints.
    setFixedHeight(height);
}

auto Modal::event(QEvent *event) -> bool {
    bool handled = qontrol::Modal::event(event);
    // A size change deep in a slot reaches the window a few events after the setter
    // that caused it: fit the window again once it does.
    if (event->type() == QEvent::LayoutRequest) {
        refit();
    }
    return handled;
}

void Modal::keep(QWidget *widget) {
    m_slots->keep(widget);
}

void closeOnlyTitlebar(QDialog *dialog) {
    dialog->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                           Qt::WindowCloseButtonHint);
}

void fixedDialog(QDialog *dialog) {
    closeOnlyTitlebar(dialog);
    dialog->layout()->setSizeConstraint(QLayout::SetFixedSize);
}

auto modalHeader(const QString &title, QWidget *end) -> QWidget * {
    auto *row = (new qontrol::Row)->push(new Label(title, LabelRole::Heading), 1);
    if (end != nullptr) {
        row->push(end);
    }
    return row;
}

} // namespace catalog
