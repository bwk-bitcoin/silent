#include "catalog/panels/receive/LabelRow.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace catalog {

LabelRow::LabelRow(QWidget *parent) : QWidget(parent) {
    setProperty("class", "label-row");

    m_add = btnAddLabel();
    m_text = new Label(LabelRole::Section, this);
    m_edit = btnEditLabel();
    m_input = new Input(InputRole::Default, this);
    m_input->setWidth(Size::M);
    m_input->setPlaceholderText(TR("receive-label-placeholder"));

    m_save = btnSaveLabel();

    (new qontrol::Row)
        ->spacing(resolve(Spacing::S))
        ->push(m_add)
        ->push(m_text)
        ->push(m_edit)
        ->push(m_input)
        ->push(m_save)
        ->into(this);

    connect(m_add, &Button::clicked, this, &LabelRow::onAddClicked, qontrol::UNIQUE);
    connect(m_edit, &Button::clicked, this, &LabelRow::onEditClicked, qontrol::UNIQUE);
    connect(m_save, &Button::clicked, this, &LabelRow::onCommit, qontrol::UNIQUE);
    connect(m_input, &QLineEdit::editingFinished, this, &LabelRow::onCommit, qontrol::UNIQUE);

    updateState();
}

void LabelRow::setLabel(const QString &label) {
    m_label = label;
    m_editing = false;
    updateState();
}

auto LabelRow::label() const -> QString {
    return m_label;
}

void LabelRow::onAddClicked() {
    m_editing = true;
    m_input->clear();
    updateState();
    m_input->setFocus();
}

void LabelRow::onEditClicked() {
    m_editing = true;
    m_input->setText(m_label);
    updateState();
    m_input->setFocus();
    m_input->selectAll();
}

void LabelRow::onCommit() {
    // Guard against re-entry: hiding the focused input below re-fires
    // editingFinished, which would call onCommit again.
    if (!m_editing) {
        return;
    }
    m_editing = false;
    const QString next = m_input->text().trimmed();
    const bool changed = next != m_label;
    m_label = next;
    updateState();
    if (changed) {
        emit labelChanged(m_label);
    }
}

void LabelRow::updateState() {
    bool hasLabel = !m_label.isEmpty();
    m_add->setVisible(!m_editing && !hasLabel);
    m_text->setVisible(!m_editing && hasLabel);
    m_edit->setVisible(!m_editing && hasLabel);
    m_input->setVisible(m_editing);
    m_save->setVisible(m_editing);
    if (hasLabel) {
        m_text->setText(m_label);
    }
}

} // namespace catalog
