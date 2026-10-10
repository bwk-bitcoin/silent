#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QGridLayout;

namespace catalog {

class FieldNote;
class Label;

// Stacked fields whose labels share one column, exactly as wide as the longest
// visible label, so their controls line up.
class FieldGroup : public QWidget {
    Q_OBJECT

public:
    explicit FieldGroup(QWidget *parent = nullptr);
    // `top` sits the label at the top of a multi-line control.
    auto addField(const QString &label, QWidget *control, bool top = false) -> FieldGroup *;
    // A note in the control column, under the field added before it.
    auto addNote(FieldNote *note) -> FieldGroup *;
    // Shows or hides a field: its control and its label.
    void setFieldVisible(QWidget *control, bool visible);

private:
    QGridLayout *m_grid = nullptr;
    QHash<QWidget *, Label *> m_labels;
};

} // namespace catalog
