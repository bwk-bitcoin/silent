#pragma once

#include "theme/Palette.h"
#include <QList>
#include <QString>
#include <Qontrol>

class QStackedWidget;

namespace catalog {

class ModalActions;
class ScrollArea;

// A modal whose steps replace each other in the same window: each step has its
// title, width, a body that scrolls when taller than the screen allows, and its
// ModalActions bar pinned under the body.
class StepModal : public qontrol::Modal {
    Q_OBJECT

public:
    StepModal();

protected:
    struct Step {
        QString title;
        ModalWidth width;
        ScrollArea *scroll;
    };

    // `actions` may be null for a transient status step. Returns the step index.
    auto addStep(const QString &title, ModalWidth width, QWidget *body, ModalActions *actions)
        -> int;
    void setStepTitle(int step, const QString &title);
    void showStep(int step);
    [[nodiscard]] auto currentStep() const -> int;

private:
    QStackedWidget *m_stack = nullptr;
    QList<Step> m_steps;
};

} // namespace catalog
