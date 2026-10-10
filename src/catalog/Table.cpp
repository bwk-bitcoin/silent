#include "catalog/Table.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/Input.h"

#include <QStyle>
#include <QVariant>
#include <Qontrol>
#include <common.h>

namespace catalog {

Table::Table(QWidget *parent) : QWidget(parent) {
    setProperty("class", "table");

    m_body = new qontrol::Column;
    m_body->setProperty("class", "table-body");

    m_scroll = new ScrollArea(this);
    // Without a fixed body height the table sizes to all its rows (no inner
    // scroll); the surrounding page scrolls instead. setBodyHeight() overrides
    // this with a fixed height when an inner scroll is wanted (e.g. coin select).
    m_scroll->setFitContent(true);
    m_scroll->setWidget(m_body);

    auto *headerRule = new Separator(Separator::Role::Horizontal, this);
    headerRule->setStrong(true);
    headerRule->setVisible(false);
    m_header_rule = headerRule;

    // A fixed bottom rule (matching the header rule), shown only when the body is
    // a bounded, scrollable window (setBodyHeight). It sits outside the scroll so
    // it stays put while the rows scroll.
    auto *footer = new Separator(Separator::Role::Horizontal, this);
    footer->setStrong(true);
    footer->setVisible(false);
    m_footer = footer;

    m_root = (new qontrol::Column)->push(m_header_rule)->push(m_scroll)->push(m_footer);
    m_root->into(this);
}

void Table::addColumn(const TableColumn &column) {
    m_columns.append(column);
    scheduleRebuild();
}

void Table::clearColumns() {
    m_columns.clear();
    scheduleRebuild();
}

void Table::addRow(const QString &id, const QStringList &cells) {
    m_rows.append(Row{.id = id, .cells = cells});
    scheduleRebuild();
}

void Table::addWidgetRow(const QString &id, const QList<QWidget *> &cells, bool bare) {
    Row row{.id = id, .bare = bare};
    for (auto *cell : cells) {
        row.widgets.append(cell);
    }
    m_rows.append(row);
    scheduleRebuild();
}

void Table::clearRows() {
    for (const auto &row : m_rows) {
        for (const auto &cell : row.widgets) {
            if (cell != nullptr) {
                cell->deleteLater();
            }
        }
    }
    m_rows.clear();
    m_selected_ids.clear();
    m_edit_row_id.clear();
    m_edit_column = -1;
    scheduleRebuild();
}

void Table::setSelectionEnabled(bool enabled) {
    if (m_selection_enabled == enabled) {
        return;
    }
    m_selection_enabled = enabled;
    scheduleRebuild();
}

void Table::setSelectedIds(const QStringList &ids) {
    m_selected_ids.clear();
    for (const auto &id : ids) {
        m_selected_ids.insert(id);
    }
    scheduleRebuild();
}

auto Table::selectedIds() const -> QStringList {
    QStringList ids;
    for (const auto &id : m_selected_ids) {
        ids.append(id);
    }
    return ids;
}

void Table::setBodyHeight(int height) {
    m_body_height = height;
    if (m_scroll != nullptr && m_body_height != width::NONE) {
        m_scroll->setFixedHeight(m_body_height);
    }
    updateScrollRules();
}

void Table::updateScrollRules() {
    bool scrollable = m_body_height != width::NONE && m_body->sizeHint().height() > m_body_height;

    if (m_header_rule != nullptr) {
        m_header_rule->setVisible(m_header != nullptr);
    }
    if (m_footer != nullptr) {
        m_footer->setVisible(scrollable);
    }
}

void Table::setHeaderVisible(bool visible) {
    m_header_visible = visible;
    scheduleRebuild();
}

void Table::onSelectionToggled(bool checked) {
    auto *checkbox = qobject_cast<Checkbox *>(sender());
    if (checkbox == nullptr) {
        return;
    }

    QString rowId = checkbox->property("rowId").toString();
    if (checked) {
        m_selected_ids.insert(rowId);
    } else {
        m_selected_ids.remove(rowId);
    }
    // Update the row's selected accent live, without a rebuild (a rebuild here would
    // delete this checkbox while its own signal is still being handled).
    if (auto *row = checkbox->parentWidget()) {
        row->setProperty("selected", checked);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
    emit rowSelectionChanged(rowId, checked);
}

void Table::onEditClicked() {
    auto *button = qobject_cast<Button *>(sender());
    if (button == nullptr) {
        return;
    }

    m_edit_row_id = button->property("rowId").toString();
    m_edit_column = button->property("column").toInt();
    scheduleRebuild();
}

void Table::onEditFinished() {
    finishEdit(true);
}

void Table::scheduleRebuild() {
    if (m_rebuild_scheduled) {
        return;
    }
    m_rebuild_scheduled = true;
    QMetaObject::invokeMethod(this, "doScheduledRebuild", Qt::QueuedConnection);
}

void Table::doScheduledRebuild() {
    m_rebuild_scheduled = false;
    rebuild();
}

void Table::rebuild() {
    // Guard against re-entrancy: a nested rebuild (e.g. from a focus change while
    // tearing down the old widgets) would build a header that this call then
    // overwrites without deleting, orphaning it in the layout.
    if (m_in_rebuild) {
        return;
    }
    m_in_rebuild = true;

    if (m_header != nullptr) {
        m_root->remove(m_header);
        m_header = nullptr;
    }

    // Hide the old rows before their deferred delete so they don't briefly paint
    // over the rebuilt ones (rebuild may run right after an inline-edit focus-out).
    for (auto &existing : m_rows) {
        if (existing.widget != nullptr) {
            existing.widget->hide();
        }
        // Widget cells outlive their old row: it is deleted, they are laid out again.
        for (const auto &cell : existing.widgets) {
            if (cell != nullptr && cell->parent() != nullptr) {
                cell->setParent(nullptr);
            }
        }
    }
    m_body->clear();

    if (m_header_visible && !m_columns.empty()) {
        m_header = buildHeader();
        m_root->insert(0, m_header);
    }

    for (int i = 0; i < m_rows.size(); ++i) {
        auto *row = buildRow(i);
        m_rows[i].widget = row;
        m_body->push(row);
        // A row added to a shown table stays hidden until the event loop shows it, so
        // the body would leave it out of the height pinned below.
        row->show();
    }

    if (m_body_height == width::NONE) {
        // Unbounded: pin the scroll to its content height so it can't shrink and
        // hand scrolling to the surrounding page scroll. A trailing stretch keeps
        // the rows top-aligned within any extra height.
        m_body->pushSpacer();
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setMinimumHeight(m_body->sizeHint().height() + 2 * m_scroll->frameWidth());
    } else {
        // Bounded: a fixed-height window with an inner scrollbar. No trailing
        // stretch here: with widgetResizable it would let the body shrink to the
        // viewport and clip the overflow rows instead of scrolling them.
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_scroll->setFixedHeight(m_body_height);
    }

    updateScrollRules();

    m_in_rebuild = false;
    emit rebuilt();
}

auto Table::buildHeader() -> QWidget * {
    auto *header = new qontrol::Row;
    header->setProperty("class", "table-header");
    header->margins(resolve(Padding::S), 0, resolve(Padding::S), metric::TABLE_CELL_PAD)
        ->spacing(resolve(Spacing::S));

    if (m_selection_enabled) {
        auto *spacer = new QWidget(header);
        // Match the Checkbox sizeHint (box + its 1px side margins) so the data
        // cells that follow line up with the header columns.
        spacer->setFixedWidth(metric::CHECKBOX_BOX + 2);
        header->push(spacer);
    }

    for (const auto &column : m_columns) {
        // Wrap each header label in a cell exactly like a body cell so grow
        // columns expand identically and the header tracks the column content.
        auto *cell = new qontrol::Row;
        auto *label = new Label(column.title, LabelRole::InfoLabel, cell);
        // Header alignment is independent of content alignment (default left).
        label->setAlignment(column.headerAlignment | Qt::AlignVCenter);
        if (column.width != width::NONE) {
            label->setFixedWidth(column.width);
        }
        cell->push(label, 0, column.headerAlignment | Qt::AlignVCenter);
        header->push(cell, column.grow ? 1 : 0);
    }

    return header;
}

auto Table::buildRow(int index) -> QWidget * {
    auto *rowWidget = new qontrol::Row(m_body);
    rowWidget->setProperty("class", "table-row");
    rowWidget->setProperty("alternate", index % 2 == 0 ? "true" : "false");
    rowWidget->setProperty("selected", m_selected_ids.contains(m_rows[index].id));
    rowWidget->setProperty("bare", m_rows[index].bare);
    rowWidget->setAttribute(Qt::WA_StyledBackground, true);
    rowWidget->setMinimumHeight(metric::TABLE_ROW_HEIGHT);

    // Vertical room comes from the row's min-height plus centering, not padding, so
    // a tall cell (e.g. the 30px edit button) fits inside the fixed row height
    // instead of growing it: history and coins rows stay the same height.
    rowWidget->margins(resolve(Padding::S), 0, resolve(Padding::S), 0)
        ->spacing(resolve(Spacing::S));

    if (m_selection_enabled) {
        auto *checkbox = new Checkbox(CheckboxRole::Default, rowWidget);
        checkbox->setProperty("rowId", m_rows[index].id);
        checkbox->setChecked(m_selected_ids.contains(m_rows[index].id));
        rowWidget->push(checkbox);
        connect(checkbox, &Checkbox::toggled, this, &Table::onSelectionToggled, qontrol::UNIQUE);
    }

    for (int i = 0; i < m_columns.size(); ++i) {
        auto *cell = buildCell(m_rows[index], i, index);
        rowWidget->push(cell, m_columns[i].grow ? 1 : 0);
    }

    return rowWidget;
}

auto Table::buildCell(const Row &row, int column, int row_index) -> QWidget * {
    if (!row.widgets.isEmpty()) {
        return buildWidgetCell(row, column);
    }
    bool editing = row.id == m_edit_row_id && column == m_edit_column;
    QString text;
    if (column < row.cells.size()) {
        text = row.cells[column];
    }

    auto *cell = new qontrol::Row;
    cell->spacing(resolve(Spacing::XS));

    if (editing) {
        auto *input = new Input(text, InputRole::Table, cell);
        input->setProperty("rowId", row.id);
        input->setProperty("column", column);
        // Take the opposite row's stripe color so the edited cell contrasts, and
        // fill the full row height so it reads as editing the cell in place.
        input->setProperty("onAlt", row_index % 2 == 0);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        cell->push(input, 1);
        input->setFocus();
        connect(input, &Input::editingFinished, this, &Table::onEditFinished, qontrol::UNIQUE);
    } else {
        auto labelRole = LabelRole::Body;
        bool monoSecondary = false;
        switch (m_columns[column].role) {
        case DisplayRole::Address:
        case DisplayRole::Outpoint:
            labelRole = LabelRole::Mono;
            monoSecondary = true;
            break;
        case DisplayRole::Amount:
        case DisplayRole::Sats:
            labelRole = LabelRole::Mono;
            break;
        case DisplayRole::Default:
            labelRole = LabelRole::Body;
            break;
        }
        auto *label = new Label(text, labelRole, cell);
        if (text.isEmpty() && !m_columns[column].placeholder.isEmpty()) {
            label->setText(m_columns[column].placeholder);
            label->setTone(LabelTone::Muted);
        }
        // Design rows are align-items:center; a horizontal-only alignment would
        // render the text top-aligned and leave a gap below it in a tall row.
        label->setAlignment(m_columns[column].alignment | Qt::AlignVCenter);
        if (monoSecondary) {
            label->setProperty("tblmono", "secondary");
        }
        QWidget *display = label;
        if (m_columns[column].grow) {
            // Flexible column: fill the cell and allow shrinking below the content
            // width so fixed columns (e.g. amount) stay visible when space is tight.
            // Text alignment is handled by the label's own setAlignment above.
            display->setMinimumWidth(0);
            cell->push(display, 1);
        } else {
            if (m_columns[column].width != width::NONE) {
                display->setFixedWidth(m_columns[column].width);
            }
            // Pin fixed-width content to its alignment edge so it matches the header.
            cell->push(display, 0, m_columns[column].alignment | Qt::AlignVCenter);
        }

        if (m_columns[column].editable) {
            auto *button = btnEditLabelRow();
            button->setProperty("rowId", row.id);
            button->setProperty("column", column);
            cell->push(button);
            connect(button, &Button::clicked, this, &Table::onEditClicked, qontrol::UNIQUE);
        }
    }

    return cell;
}

auto Table::buildWidgetCell(const Row &row, int column) -> QWidget * {
    auto *cell = new qontrol::Row;
    const auto &spec = m_columns[column];
    if (spec.width != width::NONE) {
        cell->setFixedWidth(spec.width);
    }
    QWidget *content = column < row.widgets.size() ? row.widgets[column].data() : nullptr;
    if (content == nullptr) {
        return cell;
    }
    if (spec.grow) {
        content->setMinimumWidth(0);
        // Wrapped text takes the whole column to wrap in; anything else keeps its size.
        if (content->hasHeightForWidth()) {
            cell->push(content, 1);
        } else {
            cell->push(content, 1, spec.alignment | Qt::AlignVCenter);
        }
    } else {
        cell->push(content, 0, spec.alignment | Qt::AlignVCenter);
    }
    return cell;
}

void Table::finishEdit(bool commit) {
    auto *input = qobject_cast<Input *>(sender());
    if (input == nullptr) {
        return;
    }

    QString rowId = input->property("rowId").toString();
    int column = input->property("column").toInt();
    QString text = input->text();

    if (commit) {
        for (auto &row : m_rows) {
            if (row.id == rowId && column < row.cells.size()) {
                row.cells[column] = text;
                break;
            }
        }
        emit cellEdited(rowId, column, text);
    }

    m_edit_row_id.clear();
    m_edit_column = -1;
    scheduleRebuild();
}

auto Table::qss(const Palette &p) -> QString {
    return QString("QWidget[class=\"table\"] { background: transparent; }"
                   "QWidget[class=\"table-header\"] { background: transparent; }"
                   "QWidget[class=\"table-header\"] QLabel { color: %1; }"
                   "QWidget[class=\"table-row\"] { background: %2; border-left: 2px solid "
                   "transparent; }"
                   "QWidget[class=\"table-row\"][alternate=\"true\"] { background: %3; }"
                   "QWidget[class=\"table-row\"][selected=\"true\"] { background: %2; "
                   "border-left-color: %5; }"
                   "QWidget[class=\"table-row\"]:hover { background: %4; }"
                   "QWidget[class=\"table-row\"][bare=\"true\"], "
                   "QWidget[class=\"table-row\"][bare=\"true\"]:hover { background: transparent; }"
                   "QWidget[class=\"table-row\"] QLabel[tblmono=\"secondary\"] { color: %1; }")
        .arg(p.textSecondary.name()) // %1 header cells + mono-secondary cells
        .arg(p.bg.name())            // %2 odd rows + selected
        .arg(p.bgSecondary.name())   // %3 even rows
        .arg(p.surfaceHover.name())  // %4 hover
        .arg(p.accent.name());       // %5 selection accent
}

} // namespace catalog
