#pragma once

#include "catalog/Table.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>

class QEvent;

namespace catalog {

class LabelCell;

// A table of widget rows that open on click, with editable label cells: History and
// Coins open the row's transaction.
class RowClickTable : public Table {
    Q_OBJECT

public:
    explicit RowClickTable(QWidget *parent = nullptr);
    // A click on any cell opens the row; its buttons and inputs keep their clicks.
    void addClickableRow(const QString &id, const QList<QWidget *> &cells);
    // Whether a label is being edited: the owner keeps the rows until it is done.
    [[nodiscard]] auto isEditing() const -> bool;
    // Shows `label` in row `id`'s label cell, in place.
    void setRowLabel(const QString &id, const QString &label);

signals:
    void rowClicked(const QString &row_id);
    void labelEdited(const QString &row_id, const QString &label);
    void interactionFinished();

public slots:
    void onLabelCellEdited(const QString &label);

protected:
    auto eventFilter(QObject *watched, QEvent *event) -> bool override;
    // The label cell of row `id`, reporting its edits through labelEdited.
    auto labelCell(const QString &id, const QString &label) -> LabelCell *;

private:
    QHash<QString, QPointer<LabelCell>> m_label_cells;
};

} // namespace catalog
