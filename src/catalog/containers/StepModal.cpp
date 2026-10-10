#include "catalog/containers/StepModal.h"

#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"

#include <QScreen>
#include <QStackedWidget>
#include <algorithm>

namespace catalog {

namespace {

// The window grows with its step up to this share of the screen, then the body scrolls.
constexpr qreal MAX_SCREEN_SHARE = 0.9;

} // namespace

StepModal::StepModal() {
    m_stack = new QStackedWidget;
    auto *main = (new qontrol::Column)->margins(resolve(Padding::L))->push(m_stack);
    setMainWidget(main);
}

auto StepModal::addStep(const QString &title, ModalWidth width, QWidget *body,
                        ModalActions *actions) -> int {
    auto *scroll = new ScrollArea;
    scroll->setWidget(body);
    auto *page = (new qontrol::Column)->push(scroll, 1);
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
    int maxHeight = static_cast<int>(screen()->availableGeometry().height() * MAX_SCREEN_SHARE);
    int chromeHeight = sizeHint().height() - current.scroll->sizeHint().height();
    int bodyHeight = current.scroll->widget()->sizeHint().height();
    current.scroll->setMinimumHeight(std::min(bodyHeight, maxHeight - chromeHeight));

    setWindowTitle(current.title);
    setFixedWidth(resolve(current.width));
    adjustSize();
}

auto StepModal::currentStep() const -> int {
    return m_stack->currentIndex();
}

} // namespace catalog
