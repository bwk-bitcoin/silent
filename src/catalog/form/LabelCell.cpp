#include "catalog/form/LabelCell.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Input.h"

#include <QKeyEvent>
#include <QSizePolicy>
#include <Qontrol>
#include <common.h>

namespace catalog {

LabelCell::LabelCell(const QString &label, QWidget *parent) : QWidget(parent) {
    m_text = new Label(LabelRole::Body);
    m_text->setMinimumWidth(0);
    m_text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_text->installEventFilter(this);
    m_edit = btnEditLabelRow();
    m_input = new Input(InputRole::Table);
    m_input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_input->setVisible(false);
    m_input->installEventFilter(this);

    m_row = (new qontrol::Row)
                ->spacing(resolve(Spacing::XS))
                ->push(m_text, 1)
                ->push(m_input, 1)
                ->push(m_edit);
    m_row->margins(0, 0, 0, 0);
    m_row->into(this);

    connect(m_edit, &Button::clicked, this, &LabelCell::onEditClicked, qontrol::UNIQUE);
    connect(m_input, &Input::editingFinished, this, &LabelCell::onEditFinished, qontrol::UNIQUE);
    setLabel(label);
}

void LabelCell::setLabel(const QString &label) {
    m_label = label;
    refreshText();
    m_text->setTone(label.isEmpty() ? LabelTone::Muted : LabelTone::Default);
}

void LabelCell::refreshText() {
    QString text = m_label.isEmpty() ? QStringLiteral("-") : m_label;
    m_text->setText(m_text->fontMetrics().elidedText(text, Qt::ElideRight, m_text->width()));
    m_text->setToolTip(m_label);
}

auto LabelCell::label() const -> QString {
    return m_label;
}

void LabelCell::setRetryUnchanged(bool enabled) {
    m_retry_unchanged = enabled;
}

auto LabelCell::takeEditButton() -> Button * {
    m_row->layout()->removeWidget(m_edit);
    return m_edit;
}

auto LabelCell::isEditing() const -> bool {
    return m_editing || m_committing;
}

auto LabelCell::eventFilter(QObject *watched, QEvent *event) -> bool {
    if (watched == m_text && event->type() == QEvent::Resize) {
        refreshText();
    }
    if (watched == m_input && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        showLabel();
        emit interactionFinished();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void LabelCell::showLabel() {
    // Cleared first: hiding the focused input emits editingFinished again.
    m_editing = false;
    m_input->setVisible(false);
    m_text->setVisible(true);
    m_edit->setVisible(true);
}

// ===== Slots =====

void LabelCell::onEditClicked() {
    m_editing = true;
    m_input->setText(m_label);
    m_text->setVisible(false);
    m_edit->setVisible(false);
    m_input->setVisible(true);
    m_input->setFocus();
}

void LabelCell::onEditFinished() {
    if (!m_editing) {
        return;
    }
    QString text = m_input->text();
    m_committing = true;
    showLabel();
    if (text != m_label || m_retry_unchanged) {
        setLabel(text);
        emit edited(text);
    }
    m_committing = false;
    emit interactionFinished();
}

} // namespace catalog
