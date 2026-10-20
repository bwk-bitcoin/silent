#pragma once

#include "theme/Palette.h"

#include <QList>
#include <QStringList>
#include <QWidget>

class QLabel;

namespace catalog {

class Label;

// The step trail atop a wizard modal, centred: numbered dots at a fixed pitch,
// with rules filling the remaining space;
// done steps show a check, the current one is accent and bold, later ones muted.
// Display only: the modal's Back and Continue move between steps.
class WizardSteps : public QWidget {
    Q_OBJECT

public:
    explicit WizardSteps(const QStringList &steps, QWidget *parent = nullptr);
    void setCurrent(int index);
    static auto qss(const Palette &p) -> QString;

protected:
    void refresh();

private:
    QList<QLabel *> m_dots;
    QList<Label *> m_names;
    // The rule before each step but the first.
    QList<QWidget *> m_rules;
    int m_current = 0;
};

} // namespace catalog
