#pragma once

#include "catalog/containers/Modal.h"

#include <Qontrol>
#include <silent.h>

class MusigAccountController;

namespace catalog {
class Button;
class Checkbox;
class Label;
class ModalActions;
class Table;
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

class SavedPsbts final : public qontrol::Screen {
    Q_OBJECT

public:
    explicit SavedPsbts(MusigAccountController *controller);

public slots:
    void onRefresh();
    void onListed(const SavedPsbtList &result);
    void onOpenClicked();
    void onDeleteClicked();
    void onDeleteConfirmationNeeded(const QString &id);
    void onLabelEdited();
    void onFailure(const QString &message);
    void onSaveWarningChanged(const QString &message);
    void onRetryClicked();

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void showEvent(QShowEvent *event) override;

private:
    MusigAccountController *m_controller = nullptr;
    QWidget *m_main_widget = nullptr;
    catalog::Table *m_table = nullptr;
    catalog::Checkbox *m_confirmed = nullptr;
    catalog::Label *m_error_label = nullptr;
    catalog::Label *m_warning_label = nullptr;
    catalog::Label *m_empty_label = nullptr;
    catalog::Button *m_retry_btn = nullptr;
    bool m_loading = false;
    bool m_dirty = false;
};

} // namespace musig
