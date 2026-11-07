#include "SpSigning.h"
#include "account/InterfaceEnums.h"
#include "account/PsbtTransfer.h"
#include "account/RequestId.h"
#include "account/services/Conversions.h"
#include "account/views/Send.h"

#include "AccountController.h"
#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Modal.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/MessageModal.h"
#include "i18n/Tr.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/transaction.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/sp/SpModule.h"
#include "theme/Palette.h"
#include "views/BackendErrors.h"
#include "views/utils.h"

#include <QByteArray>
#include <QDebug>
#include <QDialog>
#include <QPushButton>
#include <QThread>
#include <Qontrol>
#include <common.h>
#include <string>

using catalog::Label;

SpSigning::SpSigning(AccountController *controller, const account::Services &services)
    : m_controller(controller),
      m_services(services) {
    init();
    doConnect();
    view();
}

auto SpSigning::actions() -> QWidget * {
    return m_actions;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto SpSigning::coinsWidget() -> QWidget * {
    return nullptr;
}

void SpSigning::setTransaction(const std::optional<backend::TransactionTemplate> &tx,
                               [[maybe_unused]] const QStringList &outpoints) {
    if (m_tx_template.has_value() || !m_imported_psbt.isEmpty()) {
        return;
    }
    m_tx = tx;
    updateButtons();
}

auto SpSigning::estimatesFee() const -> bool {
    return true;
}

void SpSigning::init() {
    m_signed_pill = catalog::pillTxSigned();
    m_cancel_btn = catalog::btnCancelSigned();
    m_sign_btn = catalog::btnSign();
    m_sign_btn->setEnabled(false);
    m_broadcast_btn = catalog::btnBroadcast();
    m_export_btn = catalog::btnExportPsbt();
    m_import_btn = catalog::btnImportPsbt();
    m_status_label = new Label;
    m_status_label->setVisible(false);
}

void SpSigning::doConnect() {
    connect(m_sign_btn, &QPushButton::clicked, this, &SpSigning::onSignClicked, qontrol::UNIQUE);
    connect(m_broadcast_btn, &QPushButton::clicked, this, &SpSigning::onBroadcastClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &SpSigning::onCancelClicked,
            qontrol::UNIQUE);
    connect(m_export_btn, &QPushButton::clicked, this, &SpSigning::onExportClicked,
            qontrol::UNIQUE);
    connect(m_import_btn, &QPushButton::clicked, this, &SpSigning::onImportClicked,
            qontrol::UNIQUE);
    connect(this, &SpSigning::validationReady, this, &SpSigning::onValidationResult,
            qontrol::UNIQUE);
    connect(this, &SpSigning::signReady, this, &SpSigning::onSignResult, qontrol::UNIQUE);
    connect(m_services.blockchain, &backend::IBlockchain::broadcastFinished, this,
            &SpSigning::onBroadcastFinished, qontrol::UNIQUE);
    connect(m_services.blockchain, &backend::IService::error, this, &SpSigning::onBroadcastError,
            qontrol::UNIQUE);
    connect(m_services.transaction, &backend::IService::error, this, &SpSigning::onTransactionError,
            qontrol::UNIQUE);
}

void SpSigning::view() {
    // Spacing, not spacers: a hidden button leaves no gap.
    m_actions = (new qontrol::Row)
                    ->spacing(resolve(Spacing::S))
                    ->push(m_signed_pill)
                    ->push(m_cancel_btn)
                    ->push(m_export_btn)
                    ->push(m_sign_btn)
                    ->push(m_broadcast_btn);
    // The step's own widget, under the fee row: Import there, the fee row being full.
    auto *importRow = (new qontrol::Row)->push(m_import_btn)->pushSpacer();
    auto *col =
        (new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_status_label)->push(importRow);
    col->layout()->setContentsMargins(0, 0, 0, 0);
    setLayout(col->layout());
    updateButtons();
}

auto SpSigning::showReuseWarning(PsbtValidation result) -> bool {
    auto issues = QString::fromStdString(std::string(result.issues.c_str()));
    auto *modal = new qontrol::Modal();
    catalog::fixedDialog(modal);
    modal->setWindowTitle(TR("send-address-reuse-warning"));
    auto *label = new Label(issues + "\n\n" + TR("send-proceed-anyway"));
    label->setWordWrap(true);
    auto *proceedBtn = catalog::btnProceed();
    connect(proceedBtn, &QPushButton::clicked, modal, &QDialog::accept);
    auto *actions = new catalog::ModalActions;
    actions->addAction(proceedBtn);
    auto *col = (new qontrol::Column)->spacing(resolve(Spacing::M))->push(label)->push(actions);
    modal->setMainWidget(margin(col, resolve(Padding::L)));
    int dialogResult = modal->exec();
    delete modal;
    return dialogResult == QDialog::Accepted;
}

void SpSigning::showSignerStep() {
    auto &account = m_controller->getAccount();
    auto *keys = Host::get()->keys(SpModule::get());
    if (!account.has_value() || m_psbt.isEmpty() || keys == nullptr) {
        dropTransaction();
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("send-transaction-state-lost")));
        return;
    }

    connect(keys, &IKeys::signed_, this, &SpSigning::onSignerSigned, qontrol::UNIQUE);
    connect(keys, &IKeys::error, this, &SpSigning::onSignerError, qontrol::UNIQUE);
    connect(keys, &IKeys::cancelled, this, &SpSigning::onSignerCancelled, qontrol::UNIQUE);

    auto psbtV2Hex = QString::fromLatin1(m_psbt.toHex());
    auto descriptor = QString::fromStdString(std::string(m_controller->getDescriptor().c_str()));
    auto walletName = QString::fromStdString(std::string(account.value()->name()));
    m_sign_req_id = keys->newRequest();
    keys->sign(m_sign_req_id, account::toPublic<plugin::Network>(m_controller->network()),
               psbtV2Hex, QStringList{descriptor}, walletName, QString());
}

void SpSigning::disconnectSigner() const {
    if (auto *keys = Host::get()->keys(SpModule::get()); keys != nullptr) {
        disconnect(keys, &IKeys::signed_, this, &SpSigning::onSignerSigned);
        disconnect(keys, &IKeys::error, this, &SpSigning::onSignerError);
        disconnect(keys, &IKeys::cancelled, this, &SpSigning::onSignerCancelled);
    }
}

void SpSigning::updateButtons() {
    bool isSigned = !m_signed_tx_hex.isEmpty();
    bool imported = !m_imported_psbt.isEmpty();
    bool signable = !m_tx_template.has_value() && m_tx.has_value();
    m_signed_pill->setVisible(isSigned);
    m_cancel_btn->setVisible(isSigned || imported);
    m_sign_btn->setVisible(!isSigned);
    m_broadcast_btn->setVisible(isSigned);
    m_export_btn->setVisible(!isSigned);
    m_import_btn->setVisible(!isSigned && !imported);
    m_sign_btn->setEnabled(signable);
    m_export_btn->setEnabled(signable);
    m_import_btn->setEnabled(!m_tx_template.has_value());
}

auto SpSigning::unsignedPsbt() -> std::optional<QByteArray> {
    if (!m_imported_psbt.isEmpty()) {
        return m_imported_psbt;
    }
    if (!m_tx.has_value()) {
        return std::nullopt;
    }
    m_finalize_error.clear();
    return m_services.transaction->finalize(m_tx.value());
}

void SpSigning::restoreImported() {
    auto *send =
        qobject_cast<account::view::Send *>(m_controller->screen("send").value_or(nullptr));
    if (send == nullptr || !m_tx.has_value()) {
        return;
    }
    QList<backend::Coin> inputs;
    for (const auto &coin : m_services.blockchain->coins()) {
        if (m_tx->inputs.contains(coin.outpoint)) {
            inputs.append(coin);
        }
    }
    send->restoreTransaction(*m_tx, inputs, QString());
}

void SpSigning::dropTransaction() {
    disconnectSigner();
    m_signed_tx_hex.clear();
    m_imported_psbt.clear();
    m_psbt.clear();
    m_tx_template = std::nullopt;
    m_sign_req_id = 0;
    m_broadcast_req = 0;
    updateButtons();
    emit lockedChanged(false);
}

// ===== Slots =====

void SpSigning::onSignClicked() {
    qDebug() << "SpSigning::onSignClicked()";
    if (m_tx_template.has_value()) {
        return;
    }
    if (!m_tx.has_value()) {
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("send-invalid-transaction-template")));
        return;
    }

    // The imported PSBT, or the crafted transaction finalized on the main thread: pure
    // computation. Its reason, on failure, comes as error() during the call.
    auto psbt = unsignedPsbt();
    if (!psbt.has_value()) {
        AppController::execModal(
            new catalog::MessageModal(TR("send-prepare-failed"), failureText(m_finalize_error)));
        return;
    }

    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("send-no-account-loaded")));
        return;
    }

    // Kept for validation, signing and logging
    m_psbt = *psbt;
    m_tx_template = m_tx;
    updateButtons();
    emit lockedChanged(true);

    // Run PSBT validation on background thread before showing confirm modal
    const Account *workerAccount = &*account.value();
    auto *thread = QThread::create([this, workerAccount, psbt = m_psbt]() {
        auto result = workerAccount->validate_unsigned_psbt(account::services::toSlice(psbt));
        emit validationReady(result);
    });
    m_controller->startWorker(thread);
}

void SpSigning::onBroadcastClicked() {
    m_broadcast_btn->setEnabled(false);
    m_cancel_btn->setEnabled(false);
    m_status_label->setText(TR("confirm-send-broadcasting"));
    m_status_label->setVisible(true);
    m_broadcast_req = account::nextRequestId();
    // Registered before the request: the answer may come during the call.
    m_services.blockchain->broadcast(m_broadcast_req,
                                     QByteArray::fromHex(m_signed_tx_hex.toLatin1()));
}

void SpSigning::onCancelClicked() {
    if (!m_signed_tx_hex.isEmpty() || !m_imported_psbt.isEmpty()) {
        dropTransaction();
    }
}

void SpSigning::onExportClicked() {
    auto psbt = unsignedPsbt();
    if (!psbt.has_value()) {
        AppController::execModal(
            new catalog::MessageModal(TR("send-prepare-failed"), failureText(m_finalize_error)));
        return;
    }
    auto exported = ::export_psbt(account::services::toSlice(*psbt));
    if (!exported.is_ok) {
        AppController::execModal(new catalog::MessageModal(
            TR("common-error"), account::services::toQString(exported.error)));
        return;
    }
    AppController::execModal(new account::ExportPsbt(account::services::toQString(exported.base64),
                                                     account::services::toQString(exported.txid)));
}

void SpSigning::onImportClicked() {
    auto *modal = new account::ImportPsbt;
    m_import = modal;
    connect(modal, &account::ImportPsbt::textChosen, this, &SpSigning::onImportText,
            qontrol::UNIQUE);
    AppController::execModal(modal);
}

void SpSigning::onImportText(const QByteArray &text) {
    if (m_import == nullptr) {
        return;
    }
    auto &wallet = m_controller->getAccount();
    if (!wallet.has_value()) {
        m_import->showError(TR("send-no-account-loaded"));
        return;
    }
    auto result = wallet.value()->import_psbt(account::services::toSlice(text));
    if (!result.is_ok) {
        m_import->showError(account::services::toQString(result.error));
        return;
    }
    m_imported_psbt = account::services::toBytes(result.psbt);
    m_tx = account::services::fromTemplate(result.tx_template);
    restoreImported();
    updateButtons();
    emit lockedChanged(true);
    // Done: the transaction shows in Send.
    m_import->accept();
}

void SpSigning::onValidationResult(PsbtValidation result) {
    qDebug() << "SpSigning::onValidationResult() is_ok:" << result.is_ok
             << "is_valid:" << result.is_valid;

    if (m_psbt.isEmpty() || !m_tx_template.has_value()) {
        return;
    }

    if (!result.is_ok) {
        // Connection/protocol error: warn but don't block
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        auto text = mapBackendErrorSummary(::validation_error_kind(result)) + "\n\n" +
                    formatBackendErrorDetails(error);
        AppController::execModal(new catalog::MessageModal(TR("send-validation-error"), text));
        // Fall through and let the user confirm the transaction anyway
    } else if (result.spent_input_count > 0) {
        // Spent inputs: block the transaction
        auto issues = QString::fromStdString(std::string(result.issues.c_str()));
        AppController::execModal(new catalog::MessageModal(
            TR("common-error"), TR("send-inputs-already-spent").arg(issues)));
        dropTransaction();
        return;
    } else if (result.reused_output_count > 0) {
        // Address reuse: warn and ask user via accept/reject modal
        if (!showReuseWarning(result)) {
            dropTransaction();
            return;
        }
    }

    if (m_psbt.isEmpty() || !m_tx_template.has_value()) {
        return;
    }

    showSignerStep();
}

void SpSigning::onSignerSigned(ReqId req_id, const QString &psbt_v2_hex) {
    if (m_sign_req_id == 0 || req_id != m_sign_req_id) {
        return;
    }

    // finalize_psbt() still takes base64: convert the v2 hex the keys module
    // handed back, exactly as prepare_transaction's own bytes_hex() used to.
    auto signedPsbtBase64 =
        QString::fromLatin1(QByteArray::fromHex(psbt_v2_hex.toLatin1()).toBase64()).toStdString();
    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
        dropTransaction();
        return;
    }
    const Account *workerAccount = &*account.value();
    auto *thread = QThread::create([this, workerAccount, signedPsbtBase64]() {
        auto result = workerAccount->finalize_psbt(rust::String(signedPsbtBase64));
        emit signReady(result);
    });
    m_controller->startWorker(thread);
}

void SpSigning::onSignerError(std::optional<ReqId> req_id, const QString &message) {
    if (m_sign_req_id == 0 || !req_id.has_value() || req_id.value() != m_sign_req_id) {
        return;
    }

    disconnectSigner();
    TxResult result{};
    result.kind = BackendErrorKind::Signing;
    result.is_ok = false;
    result.error = rust::String(message.toStdString());
    result.value = rust::String();
    onSignResult(result);
}

void SpSigning::onSignerCancelled(ReqId req_id) {
    if (m_sign_req_id == 0 || req_id != m_sign_req_id) {
        return;
    }
    dropTransaction();
}

void SpSigning::onSignResult(TxResult result) {
    qDebug() << "SpSigning::onSignResult() ok:" << result.is_ok;
    disconnectSigner();

    if (!result.is_ok) {
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        dropTransaction();
        AppController::execModal(new catalog::MessageModal(TR("send-signing-failed"), error));
        return;
    }

    m_signed_tx_hex = QString::fromStdString(std::string(result.value.c_str()));
    m_psbt.clear();
    m_sign_req_id = 0;
    m_broadcast_btn->setEnabled(true);
    m_cancel_btn->setEnabled(true);
    updateButtons();
}

void SpSigning::onBroadcastFinished(backend::RequestId request_id,
                                    const backend::BroadcastResult &result) {
    if (request_id == 0 || request_id != m_broadcast_req) {
        return;
    }
    for (const auto &warning : result.warnings) {
        qWarning() << "SpSigning: broadcast" << result.txid << warning;
    }
    // Only an accepted transaction is sent; a rejected or unknown fate is a failure.
    finishBroadcast(result.outcome == backend::BroadcastOutcome::Accepted ? QString()
                                                                          : result.error);
}

void SpSigning::onBroadcastError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0 || request_id != m_broadcast_req) {
        return;
    }
    finishBroadcast(message);
}

void SpSigning::onTransactionError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0) {
        m_finalize_error = message;
    }
}

auto SpSigning::failureText(const QString &reason) -> QString {
    auto summary = operationFailedSummary();
    return reason.isEmpty() ? summary
                            : summary + QStringLiteral("\n\n") + formatBackendErrorDetails(reason);
}

void SpSigning::finishBroadcast(const QString &error) {
    qDebug() << "SpSigning::finishBroadcast() error:" << error;
    m_broadcast_req = 0;
    m_status_label->setVisible(false);
    m_broadcast_btn->setEnabled(true);
    m_cancel_btn->setEnabled(true);
    bool accepted = error.isEmpty();

    if (!accepted && m_tx_template.has_value() && !m_signed_tx_hex.isEmpty()) {
        auto &wallet = m_controller->getAccount();
        if (wallet.has_value()) {
            wallet.value()->log_failed_broadcast(account::services::toTemplate(*m_tx_template),
                                                 m_signed_tx_hex.toStdString());
        }
    }

    dropTransaction();

    if (accepted) {
        AppController::execModal(new catalog::MessageModal(
            TR("send-sent-title"), TR("send-sent-message"), catalog::MessageModal::Tone::Success));
    } else {
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("confirm-send-failed").arg(error)));
    }

    // Refresh coin state
    m_controller->pollCoins();

    if (accepted) {
        emit sent();
    }
}
