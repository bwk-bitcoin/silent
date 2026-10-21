#include "catalog/panels/home/RowClickTable.h"

#include "catalog/form/LabelCell.h"

#include <QMouseEvent>
#include <QVariant>
#include <Qontrol>
#include <algorithm>
#include <common.h>

namespace catalog {

RowClickTable::RowClickTable(QWidget *parent) : Table(parent) {
}

void RowClickTable::addClickableRow(const QString &id, const QList<QWidget *> &cells) {
    // Each cell is boxed and the box filtered, not the cell: a cell that takes its own
    // clicks (an amount cycling its mode) keeps them.
    QList<QWidget *> boxes;
    for (auto *cell : cells) {
        auto *box = (new qontrol::Row)->spacing(0)->push(cell, 1);
        box->margins(0, 0, 0, 0);
        box->setProperty("rowId", id);
        box->setCursor(Qt::PointingHandCursor);
        box->installEventFilter(this);
        boxes.append(box);
    }
    addWidgetRow(id, boxes);
}

auto RowClickTable::isEditing() const -> bool {
    return std::ranges::any_of(m_label_cells, [](const QPointer<LabelCell> &cell) {
        return cell != nullptr && cell->isEditing();
    });
}

void RowClickTable::setRowLabel(const QString &id, const QString &label) {
    auto cell = m_label_cells.value(id);
    if (cell != nullptr) {
        cell->setLabel(label);
    }
}

auto RowClickTable::labelCell(const QString &id, const QString &label) -> LabelCell * {
    auto *cell = new LabelCell(label);
    cell->setRetryUnchanged(true);
    cell->setProperty("rowId", id);
    connect(cell, &LabelCell::edited, this, &RowClickTable::onLabelCellEdited, qontrol::UNIQUE);
    connect(cell, &LabelCell::interactionFinished, this, &RowClickTable::interactionFinished,
            qontrol::UNIQUE);
    m_label_cells.insert(id, cell);
    return cell;
}

auto RowClickTable::eventFilter(QObject *watched, QEvent *event) -> bool {
    auto *cell = qobject_cast<QWidget *>(watched);
    if (cell == nullptr || !cell->property("rowId").isValid()) {
        return Table::eventFilter(watched, event);
    }
    // Only presses no child took (a label, an icon) reach the cell.
    if (event->type() == QEvent::MouseButtonPress) {
        return static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton;
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton ||
            !cell->rect().contains(cell->mapFromGlobal(mouse->globalPosition().toPoint()))) {
            return false;
        }
        emit rowClicked(cell->property("rowId").toString());
        return true;
    }
    return Table::eventFilter(watched, event);
}

// ===== Slots =====

void RowClickTable::onLabelCellEdited(const QString &label) {
    auto *cell = qobject_cast<LabelCell *>(sender());
    if (cell == nullptr) {
        return;
    }
    emit labelEdited(cell->property("rowId").toString(), label);
}

} // namespace catalog
