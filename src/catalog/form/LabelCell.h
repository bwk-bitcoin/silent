#pragma once

#include <QString>
#include <QWidget>

class QEvent;

namespace qontrol {
class Row;
}

namespace catalog {

class Button;
class Input;
class Label;

// A table cell holding an editable label: the label (a muted "-" when empty) and its
// pencil, swapped for an input while editing. Return or leaving the input commits,
// Escape cancels.
class LabelCell : public QWidget {
    Q_OBJECT

public:
    explicit LabelCell(const QString &label, QWidget *parent = nullptr);
    void setLabel(const QString &label);
    void setRetryUnchanged(bool enabled);
    // Moves the pencil into the table's separate action column.
    auto takeEditButton() -> Button *;
    [[nodiscard]] auto label() const -> QString;
    [[nodiscard]] auto isEditing() const -> bool;

signals:
    // A label was submitted; the cell already shows it. Emitted from the
    // input's own signal: a listener must not delete the cell synchronously.
    void edited(const QString &label);
    void interactionFinished();

public slots:
    void onEditClicked();
    void onEditFinished();

protected:
    auto eventFilter(QObject *watched, QEvent *event) -> bool override;
    void refreshText();
    void showLabel();

private:
    QString m_label;
    Label *m_text = nullptr;
    Button *m_edit = nullptr;
    Input *m_input = nullptr;
    qontrol::Row *m_row = nullptr;
    bool m_editing = false;
    bool m_committing = false;
    bool m_retry_unchanged = false;
};

} // namespace catalog
