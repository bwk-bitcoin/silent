#include "catalog/form/FieldGroup.h"

#include "catalog/display/Label.h"
#include "catalog/form/FieldNote.h"
#include "theme/Palette.h"

#include <QGridLayout>

namespace catalog {

FieldGroup::FieldGroup(QWidget *parent) : QWidget(parent) {
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(resolve(Spacing::XS));
    m_grid->setVerticalSpacing(resolve(Spacing::XS));
    m_grid->setColumnStretch(1, 1);
    // Takes what a fixed-width control leaves, so it stays right after its label.
    m_grid->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum), 0, 2);
}

auto FieldGroup::addField(const QString &label, QWidget *control, bool top) -> FieldGroup * {
    auto *labelWidget = new Label(label, LabelRole::Body, this);
    int row = m_grid->rowCount();
    Qt::Alignment vertical = top ? Qt::AlignTop : Qt::AlignVCenter;
    m_grid->addWidget(labelWidget, row, 0, Qt::AlignLeft | vertical);
    m_grid->addWidget(control, row, 1, vertical);
    m_labels.insert(control, labelWidget);
    return this;
}

auto FieldGroup::addNote(FieldNote *note) -> FieldGroup * {
    m_grid->addWidget(note, m_grid->rowCount(), 1);
    return this;
}

void FieldGroup::setFieldVisible(QWidget *control, bool visible) {
    control->setVisible(visible);
    auto *label = m_labels.value(control, nullptr);
    if (label != nullptr) {
        label->setVisible(visible);
    }
}

} // namespace catalog
