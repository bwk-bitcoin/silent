#include "SavedPsbts.h"

#include "AppController.h"
#include "account/PsbtTransfer.h"
#include "account/services/Conversions.h"
#include "account/views/TransactionDetails.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/InfoTip.h"
#include "catalog/display/Label.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/feedback/MessageModal.h"
#include "catalog/form/LabelCell.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/panels/home/RowClickTable.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "views/utils.h"

#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStringList>
#include <algorithm>
#include <common.h>
#include <cstdint>

namespace musig {

namespace {

constexpr int K_VALUE_W = 130;
constexpr int K_INFO_TIP_SIZE = 14;
// The status pill and its info tip.
constexpr int K_STATUS_W = resolve(PillWidth::L) + resolve(Spacing::XS) + K_INFO_TIP_SIZE;

const char *const K_SAVED_ID = "savedId";

auto text(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

auto broadcast(const SavedPsbtEntry &entry) -> bool {
    return entry.confirmed_height != 0 || entry.in_history || entry.record.broadcast;
}

// The transaction a click on the row shows: the one that replaced a deprecated draft,
// the broadcast one itself, none for a draft (it opens in Send).
auto openTxid(const SavedPsbtEntry &entry) -> QString {
    if (!entry.conflicting_txids.empty()) {
        return text(entry.conflicting_txids.front());
    }
    if (broadcast(entry)) {
        return text(entry.txid);
    }
    return {};
}

// What the recipients receive: the frozen template's outputs, MAX resolved.
auto amount(const SavedPsbtEntry &entry) -> uint64_t {
    uint64_t total = 0;
    for (const auto &output : entry.frozen_template.outputs) {
        total += output.amount;
    }
    return total;
}

auto signedText(const SavedPsbtEntry &entry, uint32_t complete) -> QString {
    if (!entry.signatures_known) {
        return QStringLiteral("-");
    }
    return QStringLiteral("%1/%2").arg(complete).arg(entry.signatures_required);
}

auto signedText(const SavedPsbtEntry &entry) -> QString {
    return signedText(entry, entry.signatures_complete);
}

// Each attempt of the entry: its id, active or not, its signatures in.
auto attemptsText(const SavedPsbtEntry &entry) -> QString {
    QStringList attempts;
    for (const auto &attempt : entry.attempts) {
        attempts.append(QStringLiteral("%1:%2:%3")
                            .arg(attempt.attempt_id)
                            .arg(static_cast<int>(attempt.active))
                            .arg(attempt.signatures_complete));
    }
    return attempts.join(QStringLiteral(","));
}

enum class PsbtStatus : std::uint8_t { Signing, Ready, Unconfirmed, Confirmed, Deprecated };

auto statusOf(const SavedPsbtEntry &entry) -> PsbtStatus {
    if (entry.confirmed_height != 0) {
        return PsbtStatus::Confirmed;
    }
    if (!entry.conflicting_txids.empty()) {
        return PsbtStatus::Deprecated;
    }
    if (broadcast(entry)) {
        return PsbtStatus::Unconfirmed;
    }
    if (!entry.record.finalized_tx_hex.empty()) {
        return PsbtStatus::Ready;
    }
    return PsbtStatus::Signing;
}

auto statusPill(PsbtStatus status) -> catalog::Badge * {
    switch (status) {
    case PsbtStatus::Signing:
        return catalog::pillPsbtSigning();
    case PsbtStatus::Ready:
        return catalog::pillPsbtReady();
    case PsbtStatus::Unconfirmed:
        return catalog::pillPsbtUnconfirmed();
    case PsbtStatus::Confirmed:
        return catalog::pillPsbtConfirmed();
    case PsbtStatus::Deprecated:
        return catalog::pillPsbtDeprecated();
    }
    return catalog::pillPsbtSigning();
}

// The entry's warning and its conflicting transactions.
auto notesOf(const SavedPsbtEntry &entry) -> QStringList {
    QStringList notes;
    if (!entry.warning.empty()) {
        notes.append(text(entry.warning));
    }
    for (const auto &conflict : entry.conflicting_txids) {
        notes.append(TR("psbt-conflict").arg(text(conflict)));
    }
    return notes;
}

// The status pill, then an info tip with the entry's notes.
auto statusCell(const SavedPsbtEntry &entry) -> QWidget * {
    auto *cell =
        (new qontrol::Row)->spacing(resolve(Spacing::XS))->push(statusPill(statusOf(entry)));
    auto notes = notesOf(entry);
    if (!notes.isEmpty()) {
        cell->push(new catalog::InfoTip(notes.join(QStringLiteral("\n"))));
    }
    return cell;
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

SavedPsbts::SavedPsbts(MusigAccountController *controller, const account::Services &services)
    : m_controller(controller),
      m_services(services) {
    init();
    doConnect();
    view();
}

void SavedPsbts::init() {
    m_table = new catalog::RowClickTable;
    m_confirmed = new catalog::Checkbox(TR("psbt-show-confirmed"));
    m_confirmed->setChecked(false);
    m_import_btn = catalog::btnImportPsbt();
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
    renderRows();
}

void SavedPsbts::doConnect() {
    connect(m_controller, &MusigAccountController::savedPsbtDeleteConfirmationNeeded, this,
            &SavedPsbts::onDeleteConfirmationNeeded, qontrol::UNIQUE);
    connect(m_confirmed, &QCheckBox::clicked, this, &SavedPsbts::onRefresh, qontrol::UNIQUE);
    connect(m_retry_btn, &QPushButton::clicked, this, &SavedPsbts::onRetryClicked, qontrol::UNIQUE);
    connect(m_import_btn, &QPushButton::clicked, this, &SavedPsbts::onImportClicked,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::psbtImported, this, &SavedPsbts::onPsbtImported,
            qontrol::UNIQUE);
    connect(m_table, &catalog::RowClickTable::rowClicked, this, &SavedPsbts::onRowClicked,
            qontrol::UNIQUE);
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
                        ->push(m_error_label)
                        ->push(m_warning_label)
                        ->push(m_retry_btn)
                        ->push(m_empty_label)
                        ->push(m_table)
                        ->pushSpacer();
    auto *titleRight =
        (new qontrol::Row)->spacing(resolve(Spacing::M))->push(m_confirmed)->push(m_import_btn);
    setScreenContent(this, m_main_widget,
                     dashboard(TR("musig-menu-psbt"), content, false, titleRight));
}

void SavedPsbts::showEvent(QShowEvent *event) {
    qontrol::Screen::showEvent(event);
    onRefresh();
}

void SavedPsbts::resizeEvent(QResizeEvent *event) {
    qontrol::Screen::resizeEvent(event);
    bool compact = catalog::compactDates(this);
    if (compact != m_compact && !held()) {
        m_compact = compact;
        renderRows();
    }
}

void SavedPsbts::renderRows() {
    m_table->clearColumns();
    m_table->clearRows();
    m_open_txids.clear();
    m_open_notes.clear();
    m_label_cells.clear();
    m_shown_rows = shownRows();
    m_table->addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    m_table->addColumn(
        {.title = TR("psbt-last-edited"), .width = catalog::dateColumnWidth(m_compact)});
    m_table->addColumn({.title = TR("psbt-signed"), .width = resolve(Size::XXS)});
    m_table->addColumn({.title = TR("coins-label"), .grow = true});
    m_table->addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    m_table->addColumn({.title = TR("psbt-status"), .width = K_STATUS_W});
    m_table->addColumn({.title = TR("coins-value"),
                        .width = K_VALUE_W,
                        .alignment = Qt::AlignRight,
                        .headerAlignment = Qt::AlignRight});
    m_table->addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    m_table->addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    m_empty_label->setVisible(m_list.is_ok);
    for (const auto &entry : m_list.entries) {
        if (entry.confirmed_height != 0 && !m_confirmed->isChecked()) {
            continue;
        }
        m_empty_label->hide();
        addRow(entry);
        if (m_unfolded.contains(text(entry.record.id))) {
            addAttemptRows(entry);
        }
    }
}

void SavedPsbts::addRow(const SavedPsbtEntry &entry) {
    auto id = text(entry.record.id);
    m_open_txids.insert(id, openTxid(entry));
    m_open_notes.insert(id, notesOf(entry));

    auto *signatures = new catalog::Label(signedText(entry), catalog::LabelRole::Mono);
    signatures->setProperty("tblmono", "secondary");

    auto *label = new catalog::LabelCell(text(entry.record.label));
    label->setProperty(K_SAVED_ID, id);
    connect(label, &catalog::LabelCell::edited, this, &SavedPsbts::onLabelEdited, qontrol::UNIQUE);
    m_label_cells.insert(id, label);

    auto *exportButton = catalog::btnExportRow();
    exportButton->setProperty(K_SAVED_ID, id);
    connect(exportButton, &QPushButton::clicked, this, &SavedPsbts::onExportClicked,
            qontrol::UNIQUE);
    auto *remove = catalog::btnDeleteRow();
    remove->setProperty(K_SAVED_ID, id);
    connect(remove, &QPushButton::clicked, this, &SavedPsbts::onDeleteClicked, qontrol::UNIQUE);
    // Each in a cell of its own: the row's click filter sits on the cells, not their
    // children.
    auto *exportCell = (new qontrol::Row)->push(exportButton);
    auto *removeCell = (new qontrol::Row)->push(remove);
    // A single attempt has nothing to fold.
    auto *foldCell = new qontrol::Row;
    if (entry.attempts.size() > 1) {
        auto *fold = catalog::btnFoldAttempts(m_unfolded.contains(id));
        fold->setProperty(K_SAVED_ID, id);
        connect(fold, &QPushButton::clicked, this, &SavedPsbts::onFoldClicked, qontrol::UNIQUE);
        foldCell->push(fold);
    }

    m_table->addClickableRow(id, {foldCell,
                                  catalog::dateStamp(entry.record.updated_at, false, m_compact),
                                  signatures, label, label->takeEditButton(), statusCell(entry),
                                  catalog::amountLabel(amount(entry)), exportCell, removeCell});
}

void SavedPsbts::addAttemptRows(const SavedPsbtEntry &entry) {
    auto id = text(entry.record.id);
    for (const auto &attempt : entry.attempts) {
        auto *signatures = new catalog::Label(signedText(entry, attempt.signatures_complete),
                                              catalog::LabelRole::Mono);
        signatures->setProperty("tblmono", "secondary");
        auto *name = new catalog::Label(
            (attempt.active ? TR("psbt-attempt-active") : TR("psbt-attempt-archived"))
                .arg(attempt.attempt_id + 1));
        name->setTone(catalog::LabelTone::Muted);
        // Under Signed and Label, the other cells empty. Not clickable: an attempt is
        // shown, never made the active one from here.
        m_table->addWidgetRow(id + QStringLiteral("/attempt/") +
                                  QString::number(attempt.attempt_id),
                              {nullptr, nullptr, signatures, name});
    }
}

auto SavedPsbts::shownRows() const -> QStringList {
    QStringList rows;
    for (const auto &entry : m_list.entries) {
        if (entry.confirmed_height != 0 && !m_confirmed->isChecked()) {
            continue;
        }
        rows.append(QStringList{text(entry.record.id), QString::number(entry.record.updated_at),
                                signedText(entry), attemptsText(entry), text(entry.record.label),
                                QString::number(static_cast<int>(statusOf(entry))),
                                notesOf(entry).join(QStringLiteral("\n")),
                                QString::number(amount(entry)), openTxid(entry)}
                        .join(QStringLiteral("\t")));
    }
    return rows;
}

void SavedPsbts::execOverRows(QDialog *modal) {
    m_modal_open = true;
    QPointer<SavedPsbts> self(this);
    AppController::execModal(modal);
    if (self != nullptr) {
        m_modal_open = false;
    }
}

auto SavedPsbts::held() const -> bool {
    if (m_modal_open) {
        return true;
    }
    return std::ranges::any_of(
        m_label_cells, [](const auto &cell) { return cell != nullptr && cell->isEditing(); });
}

// ===== Slots =====

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
    // Dropped: the next poll or label edit lists them again.
    if (held()) {
        m_dirty = true;
        return;
    }
    if (result.is_ok) {
        m_error_label->hide();
    } else {
        onFailure(text(result.error));
    }
    bool wasOk = m_list.is_ok;
    m_list = result;
    bool compact = catalog::compactDates(this);
    if (compact != m_compact || wasOk != m_list.is_ok || shownRows() != m_shown_rows) {
        m_compact = compact;
        renderRows();
    }
    if (m_dirty) {
        onRefresh();
    }
}

void SavedPsbts::onRowClicked(const QString &id) {
    auto txid = m_open_txids.value(id);
    if (txid.isEmpty()) {
        // The controller shows it in Send.
        m_controller->restoreSavedPsbt(id);
        return;
    }
    // The rows stay while the details run over them.
    m_modal_open = true;
    QPointer<SavedPsbts> self(this);
    account::showTransactionDetails(m_services, txid, m_open_notes.value(id));
    if (self != nullptr) {
        m_modal_open = false;
    }
}

void SavedPsbts::onFoldClicked() {
    auto id = sender()->property(K_SAVED_ID).toString();
    if (!m_unfolded.remove(id)) {
        m_unfolded.insert(id);
    }
    // The fold button is deleted later, once this click is handled.
    renderRows();
}

void SavedPsbts::onDeleteClicked() {
    m_controller->deleteSavedPsbt(sender()->property(K_SAVED_ID).toString());
}

void SavedPsbts::onExportClicked() {
    auto id = sender()->property(K_SAVED_ID).toString();
    for (const auto &entry : m_list.entries) {
        if (text(entry.record.id) != id) {
            continue;
        }
        auto exported = ::export_psbt(account::services::toSlice(
            QByteArray::fromHex(text(entry.record.current_psbt_v2_hex).toLatin1())));
        if (!exported.is_ok) {
            execOverRows(new catalog::MessageModal(TR("common-error"), text(exported.error)));
            return;
        }
        // Named after its label, else its transaction.
        auto name = entry.record.label.empty() ? text(entry.txid) : text(entry.record.label);
        execOverRows(new account::ExportPsbt(text(exported.base64), name));
        return;
    }
}

void SavedPsbts::onImportClicked() {
    auto *modal = new account::ImportPsbt;
    m_import = modal;
    connect(modal, &account::ImportPsbt::textChosen, this, &SavedPsbts::onImportText,
            qontrol::UNIQUE);
    execOverRows(modal);
}

void SavedPsbts::onImportText(const QByteArray &contents) {
    m_controller->importSavedPsbt(contents);
}

void SavedPsbts::onPsbtImported(const SavedPsbtImport &result) {
    if (m_import == nullptr) {
        return;
    }
    if (!result.is_ok) {
        m_import->showError(text(result.error));
        return;
    }
    m_import->showImported(result.merged ? TR("psbt-import-merged") : TR("psbt-import-saved"));
}

void SavedPsbts::onDeleteConfirmationNeeded(const QString &id) {
    auto *confirm = new ConfirmPsbtDelete(id);
    connect(confirm, &ConfirmPsbtDelete::confirmed, m_controller,
            &MusigAccountController::onSavedPsbtDeleteConfirmed, qontrol::UNIQUE);
    connect(confirm, &ConfirmPsbtDelete::cancelled, m_controller,
            &MusigAccountController::onSavedPsbtDeleteCancelled, qontrol::UNIQUE);
    execOverRows(confirm);
}

void SavedPsbts::onLabelEdited(const QString &label) {
    m_controller->labelSavedPsbt(sender()->property(K_SAVED_ID).toString(), label);
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
