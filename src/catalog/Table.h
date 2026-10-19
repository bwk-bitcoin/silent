#pragma once

#include "catalog/display/Display.h"
#include "theme/Palette.h"
#include <QList>
#include <QPointer>
#include <QSet>
#include <QWidget>

namespace qontrol {
class Column;
}

namespace catalog {

class ScrollArea;

struct TableColumn {
    QString title;
    int width = width::NONE;
    Qt::Alignment alignment = Qt::AlignLeft;       // content alignment
    Qt::Alignment headerAlignment = Qt::AlignLeft; // header alignment (independent)
    DisplayRole role = DisplayRole::Default;
    bool grow = false;
    bool editable = false;
    // Shown muted in an empty cell; an edit still starts from the empty text.
    QString placeholder;
};

class Table : public QWidget {
    Q_OBJECT

public:
    explicit Table(QWidget *parent = nullptr);
    void addColumn(const TableColumn &column);
    void clearColumns();
    void addRow(const QString &id, const QStringList &cells);
    // A row of widgets, one per column, owned by the table from here on. A bare row
    // (the + of an add row) has no fill and no hover.
    void addWidgetRow(const QString &id, const QList<QWidget *> &cells, bool bare = false);
    void clearRows();
    void setSelectionEnabled(bool enabled);
    void setSelectedIds(const QStringList &ids);
    [[nodiscard]] auto selectedIds() const -> QStringList;
    void setBodyHeight(int height);
    void setHeaderVisible(bool visible);
    static auto qss(const Palette &p) -> QString;

signals:
    void rowSelectionChanged(const QString &row_id, bool selected);
    void cellEdited(const QString &row_id, int column, const QString &text);
    // The rows were laid out again: the table may have changed height.
    void rebuilt();

public slots:
    void onSelectionToggled(bool checked);
    void onEditClicked();
    void onEditFinished();
    // Runs a coalesced rebuild posted by scheduleRebuild().
    void doScheduledRebuild();
    // Emits cellEdited, posted by finishEdit() once the edit's own call stack is done:
    // a listener may rebuild the screen and delete this table (ws-plaude/silent#42).
    void emitCellEdited(const QString &row_id, int column, const QString &text);

private:
    struct Row {
        QString id;
        QStringList cells;
        QList<QPointer<QWidget>> widgets;
        bool bare = false;
        QWidget *widget = nullptr;
    };

    void rebuild();
    // Posts a single rebuild to the event loop, coalescing multiple requests and
    // ensuring the rebuild runs after the current event (a focus-out or click on
    // an edited cell) has fully unwound, so widgets are never deleted mid-event.
    void scheduleRebuild();
    void updateScrollRules();
    auto buildHeader() -> QWidget *;
    auto buildRow(int index) -> QWidget *;
    auto buildCell(const Row &row, int column, int row_index) -> QWidget *;
    auto buildWidgetCell(const Row &row, int column) -> QWidget *;
    void finishEdit(bool commit);

    QList<TableColumn> m_columns;
    QList<Row> m_rows;
    QSet<QString> m_selected_ids;
    QWidget *m_header = nullptr;
    QWidget *m_header_rule = nullptr;
    qontrol::Column *m_body = nullptr;
    QWidget *m_footer = nullptr;
    ScrollArea *m_scroll = nullptr;
    qontrol::Column *m_root = nullptr;
    bool m_selection_enabled = false;
    bool m_header_visible = true;
    bool m_rebuild_scheduled = false;
    bool m_in_rebuild = false;
    int m_body_height = width::NONE;
    QString m_edit_row_id;
    int m_edit_column = -1;
};

} // namespace catalog
