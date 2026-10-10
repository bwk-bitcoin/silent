#pragma once

#include "theme/Palette.h"
#include <QList>
#include <QString>
#include <Qontrol>

class QShowEvent;
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
    // A step shown before the window was polished measured itself unstyled: measure
    // it again once the window shows.
    void showEvent(QShowEvent *event) override;

    struct Step {
        QString title;
        ModalWidth width;
        ScrollArea *scroll;
    };

    // `actions` may be null for a transient status step. Returns the step index.
    auto addStep(const QString &title, ModalWidth width, QWidget *body, ModalActions *actions)
        -> int;
    void setStepTitle(int step, const QString &title);
    // Pinned on the right above every step (the window title is the system's): a
    // fingerprint, a status pill. Null clears it.
    void setHeaderEnd(QWidget *widget);
    void setHeaderEndVisible(bool visible);
    void showStep(int step);
    [[nodiscard]] auto currentStep() const -> int;
    // Scrolls the shown step so `widget`, in its body, starts at the top of the area.
    void scrollTo(QWidget *widget);

private:
    QStackedWidget *m_stack = nullptr;
    qontrol::Row *m_header = nullptr;
    QWidget *m_header_end = nullptr;
    QList<Step> m_steps;
};

} // namespace catalog
