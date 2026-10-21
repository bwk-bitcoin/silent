#pragma once

#include "catalog/containers/Modal.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <Qontrol>
#include <silent.h>

class MusigAccountController;
class QResizeEvent;

namespace catalog {
class Button;
class Checkbox;
class Label;
class LabelCell;
class ModalActions;
class RowClickTable;
} // namespace catalog

namespace musig {

class ConfirmPsbtDelete final : public catalog::Modal {
    Q_OBJECT

public:
    explicit ConfirmPsbtDelete(const QString &id);

signals:
    void confirmed(QString id);
    void cancelled(QString id);

public slots:
    void onDeleteClicked();
    void onFinished(int result);

protected:
    void init();
    void doConnect();
    void view();

private:
    QString m_id;
    catalog::Button *m_delete_btn = nullptr;
    catalog::Label *m_message_label = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

// The Multikey PSBT tab: one row per saved transaction, last edited, signatures in,
// label, status, amount. A click on a row opens it: a draft in Send, a broadcast one in
// its transaction details, a deprecated one in the details of the transaction that
// replaced it.
class SavedPsbts final : public qontrol::Screen {
    Q_OBJECT

public:
    explicit SavedPsbts(MusigAccountController *controller);

public slots:
    void onRefresh();
    void onListed(const SavedPsbtList &result);
    void onRowClicked(const QString &id);
    void onDeleteClicked();
    void onDeleteConfirmationNeeded(const QString &id);
    void onLabelEdited(const QString &label);
    void onFailure(const QString &message);
    void onSaveWarningChanged(const QString &message);
    void onRetryClicked();

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // The table of m_list, its dates compact or not.
    void renderRows();
    void addRow(const SavedPsbtEntry &entry);
    // What the rows of m_list show, one line per row shown.
    [[nodiscard]] auto shownRows() const -> QStringList;
    // A label is being edited or a modal runs over the rows: they are not rebuilt.
    [[nodiscard]] auto held() const -> bool;

private:
    MusigAccountController *m_controller = nullptr;
    QWidget *m_main_widget = nullptr;
    catalog::RowClickTable *m_table = nullptr;
    catalog::Checkbox *m_confirmed = nullptr;
    catalog::Label *m_error_label = nullptr;
    catalog::Label *m_warning_label = nullptr;
    catalog::Label *m_empty_label = nullptr;
    catalog::Button *m_retry_btn = nullptr;
    SavedPsbtList m_list{};
    // The transaction a click on a row shows, by saved PSBT id: empty for a draft.
    QHash<QString, QString> m_open_txids;
    // The warning and conflicts of each row, shown with its transaction.
    QHash<QString, QStringList> m_open_notes;
    QHash<QString, QPointer<catalog::LabelCell>> m_label_cells;
    QStringList m_shown_rows;
    bool m_compact = false;
    bool m_modal_open = false;
    bool m_loading = false;
    bool m_dirty = false;
};

} // namespace musig
