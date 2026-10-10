#include "catalog/containers/StepModal.h"

#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"

#include <QLayout>
#include <QScreen>
#include <QScrollBar>
#include <QShowEvent>
#include <QStackedWidget>
#include <algorithm>

namespace catalog {

namespace {

// The window grows with its step up to this share of the screen, then the body scrolls.
constexpr qreal MAX_SCREEN_SHARE = 0.9;

} // namespace

StepModal::StepModal() {
    m_stack = new QStackedWidget;
    m_header = (new qontrol::Row)->pushSpacer();
    m_header->setVisible(false);
    auto *main = (new qontrol::Column)
                     ->margins(resolve(Padding::L))
                     ->spacing(resolve(Spacing::S))
                     ->push(m_header)
                     ->push(m_stack);
    setMainWidget(main);
}

void StepModal::setHeaderEnd(QWidget *widget) {
    if (m_header_end != nullptr) {
        m_header->remove(m_header_end);
    }
    m_header_end = widget;
    if (m_header_end != nullptr) {
        m_header->push(m_header_end);
    }
    m_header->setVisible(m_header_end != nullptr);
}

void StepModal::setHeaderEndVisible(bool visible) {
    m_header->setVisible(visible && m_header_end != nullptr);
}

auto StepModal::addStep(const QString &title, ModalWidth width, QWidget *body,
                        ModalActions *actions) -> int {
    auto *scroll = new ScrollArea;
    scroll->setFitContent(true);
    // The stretch keeps the body packed at the top of a step taller than it.
    scroll->setWidget((new qontrol::Column)->push(body)->pushStretch(1));
    // The step's own gap before its actions bar (design Stack gap m).
    auto *page = (new qontrol::Column)->spacing(resolve(Spacing::M))->push(scroll, 1);
    if (actions != nullptr) {
        page->push(actions);
    }
    m_steps.append({.title = title, .width = width, .scroll = scroll});
    return m_stack->addWidget(page);
}

void StepModal::setStepTitle(int step, const QString &title) {
    m_steps[step].title = title;
    if (step == m_stack->currentIndex()) {
        setWindowTitle(title);
    }
}

void StepModal::showStep(int step) {
    // Only the shown page counts toward the window size, so each step sizes to itself.
    for (int index = 0; index < m_stack->count(); ++index) {
        auto policy = index == step ? QSizePolicy::Preferred : QSizePolicy::Ignored;
        m_stack->widget(index)->setSizePolicy(policy, policy);
    }
    m_stack->setCurrentIndex(step);

    const auto &current = m_steps.at(step);
    setWindowTitle(current.title);
    setFixedWidth(resolve(current.width));
    // Free of the height fixed last time, so the window hint is the chrome plus the
    // area's own hint.
    current.scroll->setMinimumHeight(0);
    current.scroll->setMaximumHeight(QWIDGETSIZE_MAX);
    // A child shown or hidden since the last pass leaves the hints cached above it
    // stale, and the window keeps the minimum height of the last pass: drop them all.
    for (auto *child : findChildren<QWidget *>()) {
        child->updateGeometry();
    }
    layout()->invalidate();
    // Lays the step out at its width, so wrapped text reports the height it needs.
    layout()->activate();

    int maxHeight = static_cast<int>(screen()->availableGeometry().height() * MAX_SCREEN_SHARE);
    int scrollHint =
        current.scroll->sizeHint().expandedTo(current.scroll->minimumSizeHint()).height();
    // The stack hints at its tallest step, whatever their size policies: take the
    // shown step's own hint in its place.
    int pageHint = m_stack->currentWidget()->sizeHint().height();
    int chromeHeight = sizeHint().height() - m_stack->sizeHint().height() + pageHint - scrollHint;
    auto *body = current.scroll->widget();
    int bodyWidth = current.scroll->viewport()->width();
    int bodyHeight =
        body->hasHeightForWidth() ? body->heightForWidth(bodyWidth) : body->sizeHint().height();
    // The area's edge lines take their height too, or the last pixels scroll.
    int scrollHeight =
        std::min(bodyHeight + 2 * current.scroll->frameWidth(), maxHeight - chromeHeight);
    current.scroll->setMinimumHeight(scrollHeight);
    current.scroll->setMaximumHeight(maxHeight - chromeHeight);
    // Not adjustSize(): it sizes wrapped text at the hint width, and the stack's
    // height for width is the tallest step's.
    resize(resolve(current.width), chromeHeight + scrollHeight);
}

void StepModal::showEvent(QShowEvent *event) {
    qontrol::Modal::showEvent(event);
    if (!m_steps.isEmpty()) {
        showStep(currentStep());
    }
}

auto StepModal::currentStep() const -> int {
    return m_stack->currentIndex();
}

void StepModal::scrollTo(QWidget *widget) {
    auto *scroll = m_steps.at(currentStep()).scroll;
    scroll->verticalScrollBar()->setValue(widget->mapTo(scroll->widget(), QPoint(0, 0)).y());
}

} // namespace catalog
