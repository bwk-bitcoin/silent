#include "SavedPsbts.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "views/utils.h"

#include <QShowEvent>
#include <common.h>

namespace musig {

namespace {

auto text(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

auto statusText(const SavedPsbtEntry &entry) -> QString {
    if (entry.confirmed_height != 0) {
        return TR("psbt-confirmed").arg(entry.confirmed_height);
    }
    if (entry.in_history || entry.record.broadcast) {
        return TR("psbt-broadcast");
    }
    if (!entry.record.finalized_tx_hex.empty()) {
        return TR("psbt-finalized");
    }
    return TR("psbt-signing");
}

auto statusView(const SavedPsbtEntry &entry) -> QWidget * {
    auto *column = (new qontrol::Column)->spacing(resolve(Spacing::XXS));
    auto *state = new catalog::Label(statusText(entry));
    state->setWordWrap(true);
    column->push(state);
    if (!entry.warning.empty()) {
        auto *warning = new catalog::Label(text(entry.warning));
        warning->setWordWrap(true);
        column->push(warning);
    }
    for (const auto &conflict : entry.conflicting_txids) {
        column->push(new catalog::Label(TR("psbt-conflict").arg(QString())));
        auto *txid = new catalog::Label;
        txid->setTextWrapAnywhere(text(conflict));
        column->push(txid);
    }
    return column;
}

} // namespace

ConfirmPsbtDelete::ConfirmPsbtDelete(const QString &id) : m_id(id) {
    init();
    doConnect();
    view();
}

void ConfirmPsbtDelete::init() {
    m_delete_btn = catalog::btnDelete();
    m_message_label = new catalog::Label(TR("psbt-delete-message"));
    m_message_label->setWordWrap(true);
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_delete_btn);
}

void ConfirmPsbtDelete::doConnect() {
    connect(m_delete_btn, &QPushButton::clicked, this, &ConfirmPsbtDelete::onDeleteClicked,
            qontrol::UNIQUE);
    connect(this, &QDialog::finished, this, &ConfirmPsbtDelete::onFinished, qontrol::UNIQUE);
}

void ConfirmPsbtDelete::view() {
    setWindowTitle(TR("psbt-delete-title"));
    setBody(m_message_label);
    setFooter(m_actions);
}

void ConfirmPsbtDelete::onDeleteClicked() {
    emit confirmed(m_id);
    accept();
}

void ConfirmPsbtDelete::onFinished(int result) {
    if (result != QDialog::Accepted) {
        emit cancelled(m_id);
    }
}

SavedPsbts::SavedPsbts(MusigAccountController *controller) : m_controller(controller) {
    init();
    doConnect();
    view();
}

void SavedPsbts::init() {
    m_table = new catalog::Table;
    m_table->addColumn({.title = TR("psbt-transaction"), .width = resolve(Size::S)});
    m_table->addColumn({.title = TR("coins-label"), .width = resolve(Size::M)});
    m_table->addColumn({.title = TR("psbt-status"), .grow = true});
    m_table->addColumn({.title = QString(), .width = resolve(BtnWidth::M)});
    m_confirmed = new catalog::Checkbox(TR("psbt-show-confirmed"));
    m_confirmed->setChecked(false);
    m_error_label = new catalog::Label;
    m_error_label->setWordWrap(true);
    m_error_label->hide();
    m_warning_label = new catalog::Label;
    m_warning_label->setWordWrap(true);
    m_warning_label->hide();
    m_empty_label = new catalog::Label(TR("psbt-empty"));
    m_retry_btn = catalog::btnTryAgain();
    m_retry_btn->setText(TR("psbt-retry-save"));
    m_retry_btn->hide();
    onSaveWarningChanged(m_controller->saveWarning());
}

void SavedPsbts::doConnect() {
    connect(m_controller, &MusigAccountController::savedPsbtDeleteConfirmationNeeded, this,
            &SavedPsbts::onDeleteConfirmationNeeded, qontrol::UNIQUE);
    connect(m_confirmed, &QCheckBox::clicked, this, &SavedPsbts::onRefresh, qontrol::UNIQUE);
    connect(m_retry_btn, &QPushButton::clicked, this, &SavedPsbts::onRetryClicked, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::savedPsbtsChanged, this, &SavedPsbts::onRefresh,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::savedPsbtsListed, this, &SavedPsbts::onListed,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::savedPsbtFailed, this, &SavedPsbts::onFailure,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::saveWarningChanged, this,
            &SavedPsbts::onSaveWarningChanged, qontrol::UNIQUE);
}

void SavedPsbts::view() {
    auto *content = (new qontrol::Column)
                        ->spacing(resolve(Spacing::S))
                        ->push(m_confirmed)
                        ->push(m_error_label)
                        ->push(m_warning_label)
                        ->push(m_retry_btn)
                        ->push(m_empty_label)
                        ->push(m_table)
                        ->pushSpacer();
    setScreenContent(this, m_main_widget, dashboard(TR("musig-menu-psbt"), content));
}

void SavedPsbts::showEvent(QShowEvent *event) {
    qontrol::Screen::showEvent(event);
    onRefresh();
}

void SavedPsbts::onRefresh() {
    if (!isVisible()) {
        return;
    }
    if (m_loading) {
        m_dirty = true;
        return;
    }
    m_loading = true;
    m_dirty = false;
    m_controller->listSavedPsbts(m_confirmed->isChecked());
}

void SavedPsbts::onListed(const SavedPsbtList &result) {
    m_loading = false;
    if (!result.is_ok) {
        onFailure(text(result.error));
    }
    m_table->clearRows();
    m_empty_label->setVisible(result.is_ok);
    for (const auto &entry : result.entries) {
        if (entry.confirmed_height != 0 && !m_confirmed->isChecked()) {
            continue;
        }
        auto id = text(entry.record.id);
        m_empty_label->hide();
        auto *open = catalog::btnOpenSavedPsbt(text(entry.txid));
        open->setProperty("savedId", id);
        connect(open, &QPushButton::clicked, this, &SavedPsbts::onOpenClicked, qontrol::UNIQUE);
        auto *label = new catalog::Input;
        label->setWidth(Size::M);
        label->setText(text(entry.record.label));
        label->setProperty("savedId", id);
        connect(label, &QLineEdit::editingFinished, this, &SavedPsbts::onLabelEdited,
                qontrol::UNIQUE);
        auto *status = statusView(entry);
        auto *remove = catalog::btnDelete();
        remove->setProperty("savedId", id);
        connect(remove, &QPushButton::clicked, this, &SavedPsbts::onDeleteClicked, qontrol::UNIQUE);
        m_table->addWidgetRow(id, {open, label, status, remove});
    }
    if (m_dirty) {
        onRefresh();
    }
}

void SavedPsbts::onOpenClicked() {
    m_controller->restoreSavedPsbt(sender()->property("savedId").toString());
}

void SavedPsbts::onDeleteClicked() {
    m_controller->deleteSavedPsbt(sender()->property("savedId").toString());
}

void SavedPsbts::onDeleteConfirmationNeeded(const QString &id) {
    auto *confirm = new ConfirmPsbtDelete(id);
    connect(confirm, &ConfirmPsbtDelete::confirmed, m_controller,
            &MusigAccountController::onSavedPsbtDeleteConfirmed, qontrol::UNIQUE);
    connect(confirm, &ConfirmPsbtDelete::cancelled, m_controller,
            &MusigAccountController::onSavedPsbtDeleteCancelled, qontrol::UNIQUE);
    AppController::execModal(confirm);
}

void SavedPsbts::onLabelEdited() {
    auto *input = qobject_cast<catalog::Input *>(sender());
    if (input == nullptr || !input->isModified()) {
        return;
    }
    input->setModified(false);
    m_controller->labelSavedPsbt(input->property("savedId").toString(), input->text());
}

void SavedPsbts::onFailure(const QString &message) {
    m_error_label->setText(message);
    m_error_label->setVisible(true);
}

void SavedPsbts::onSaveWarningChanged(const QString &message) {
    m_warning_label->setText(TR("psbt-save-warning").arg(message));
    m_warning_label->setVisible(!message.isEmpty());
    m_retry_btn->setVisible(!message.isEmpty());
}

void SavedPsbts::onRetryClicked() {
    m_controller->retrySavedPsbts();
}

} // namespace musig
