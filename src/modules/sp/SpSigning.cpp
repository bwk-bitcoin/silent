#include "SpSigning.h"

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
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "theme/Palette.h"
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

SpSigning::SpSigning(AccountController *controller) : m_controller(controller) {
    init();
    doConnect();
    view();
}

auto SpSigning::actions() -> QWidget * {
    return m_actions;
}

void SpSigning::setTransaction(const std::optional<TransactionTemplate> &tx,
                               [[maybe_unused]] const QStringList &outpoints) {
    m_tx = tx;
    m_sign_btn->setEnabled(m_tx.has_value());
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
    m_status_label = new Label;
    m_status_label->setVisible(false);
}

void SpSigning::doConnect() {
    connect(m_sign_btn, &QPushButton::clicked, this, &SpSigning::onSignClicked, qontrol::UNIQUE);
    connect(m_broadcast_btn, &QPushButton::clicked, this, &SpSigning::onBroadcastClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &SpSigning::onCancelClicked,
            qontrol::UNIQUE);
    connect(this, &SpSigning::validationReady, this, &SpSigning::onValidationResult,
            qontrol::UNIQUE);
    connect(this, &SpSigning::signReady, this, &SpSigning::onSignResult, qontrol::UNIQUE);
    connect(this, &SpSigning::broadcastReady, this, &SpSigning::onBroadcastResult, qontrol::UNIQUE);
}

void SpSigning::view() {
    m_actions = (new qontrol::Row)
                    ->push(m_signed_pill)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(m_cancel_btn)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(m_sign_btn)
                    ->push(m_broadcast_btn);
    auto *col = (new qontrol::Column)->push(m_status_label);
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
    if (dialogResult != QDialog::Accepted) {
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        return false;
    }
    return true;
}

void SpSigning::showSignerStep() {
    auto &account = m_controller->getAccount();
    if (!account.has_value() || !m_psbt_result.has_value() || Host::get()->keys() == nullptr) {
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("send-transaction-state-lost")));
        return;
    }

    connect(Host::get()->keys(), &IKeys::signed_, this, &SpSigning::onSignerSigned,
            qontrol::UNIQUE);
    connect(Host::get()->keys(), &IKeys::error, this, &SpSigning::onSignerError, qontrol::UNIQUE);
    connect(Host::get()->keys(), &IKeys::cancelled, this, &SpSigning::onSignerCancelled,
            qontrol::UNIQUE);

    auto psbtV2Hex =
        QString::fromStdString(std::string(m_psbt_result.value()->bytes_v2_hex().c_str()));
    auto descriptor = QString::fromStdString(std::string(m_controller->getDescriptor().c_str()));
    auto walletName = QString::fromStdString(std::string(account.value()->name()));
    m_sign_req_id = Host::get()->keys()->newRequest();
    Host::get()->keys()->sign(m_sign_req_id, m_controller->network(), psbtV2Hex,
                              QStringList{descriptor}, walletName);
}

void SpSigning::disconnectSigner() const {
    if (Host::get()->keys() != nullptr) {
        disconnect(Host::get()->keys(), &IKeys::signed_, this, &SpSigning::onSignerSigned);
        disconnect(Host::get()->keys(), &IKeys::error, this, &SpSigning::onSignerError);
        disconnect(Host::get()->keys(), &IKeys::cancelled, this, &SpSigning::onSignerCancelled);
    }
}

void SpSigning::updateButtons() {
    bool isSigned = !m_signed_tx_hex.isEmpty();
    m_signed_pill->setVisible(isSigned);
    m_cancel_btn->setVisible(isSigned);
    m_sign_btn->setVisible(!isSigned);
    m_broadcast_btn->setVisible(isSigned);
}

void SpSigning::dropSigned() {
    m_signed_tx_hex.clear();
    m_tx_template = std::nullopt;
    updateButtons();
    emit lockedChanged(false);
}

// ===== Slots =====

void SpSigning::onSignClicked() {
    qDebug() << "SpSigning::onSignClicked()";
    if (!m_tx.has_value()) {
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("send-invalid-transaction-template")));
        return;
    }

    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("send-no-account-loaded")));
        return;
    }

    // Prepare transaction on main thread (pure computation)
    auto psbt = account.value()->prepare_transaction(m_tx.value());
    if (!psbt->is_ok()) {
        auto error = QString::fromStdString(std::string(psbt->get_psbt_error().c_str()));
        auto text = mapBackendErrorSummary(error) + "\n\n" + formatBackendErrorDetails(error);
        AppController::execModal(new catalog::MessageModal(TR("send-prepare-failed"), text));
        return;
    }

    // Store psbt and tx template for use in validation, signing and logging
    m_psbt_result = std::make_optional(std::move(psbt));
    m_tx_template = m_tx;

    // Run PSBT validation on background thread before showing confirm modal
    m_sign_btn->setEnabled(false);
    const Account *workerAccount = &*account.value();
    auto *thread = QThread::create([this, workerAccount]() {
        auto result = workerAccount->validate_before_sign(*m_psbt_result.value());
        emit validationReady(result);
    });
    m_controller->startWorker(thread);
}

void SpSigning::onBroadcastClicked() {
    m_broadcast_btn->setEnabled(false);
    m_cancel_btn->setEnabled(false);
    m_status_label->setText(TR("confirm-send-broadcasting"));
    m_status_label->setVisible(true);
    auto signedHex = m_signed_tx_hex.toStdString();
    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
        return;
    }
    const Account *workerAccount = &*account.value();
    auto *thread = QThread::create([this, workerAccount, signedHex]() {
        auto broadcastResult = workerAccount->broadcast_transaction(rust::String(signedHex));
        emit broadcastReady(broadcastResult);
    });
    m_controller->startWorker(thread);
}

void SpSigning::onCancelClicked() {
    dropSigned();
}

void SpSigning::onValidationResult(PsbtValidation result) {
    qDebug() << "SpSigning::onValidationResult() is_ok:" << result.is_ok
             << "is_valid:" << result.is_valid;

    m_sign_btn->setEnabled(m_tx.has_value());

    if (!result.is_ok) {
        // Connection/protocol error: warn but don't block
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        auto text = mapBackendErrorSummary(error) + "\n\n" + formatBackendErrorDetails(error);
        AppController::execModal(new catalog::MessageModal(TR("send-validation-error"), text));
        // Fall through and let the user confirm the transaction anyway
    } else if (result.spent_input_count > 0) {
        // Spent inputs: block the transaction
        auto issues = QString::fromStdString(std::string(result.issues.c_str()));
        AppController::execModal(new catalog::MessageModal(
            TR("common-error"), TR("send-inputs-already-spent").arg(issues)));
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        return;
    } else if (result.reused_output_count > 0) {
        // Address reuse: warn and ask user via accept/reject modal
        if (!showReuseWarning(result)) {
            return;
        }
    }

    if (!m_psbt_result.has_value() || !m_tx_template.has_value()) {
        return;
    }

    showSignerStep();
}

void SpSigning::onSignerSigned(ReqId req_id, const QString &psbt_v2_hex) {
    if (req_id != m_sign_req_id) {
        return;
    }

    // finalize_psbt() still takes base64: convert the v2 hex the keys module
    // handed back, exactly as prepare_transaction's own bytes_hex() used to.
    auto signedPsbtBase64 =
        QString::fromLatin1(QByteArray::fromHex(psbt_v2_hex.toLatin1()).toBase64()).toStdString();
    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
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
    if (!req_id.has_value() || req_id.value() != m_sign_req_id) {
        return;
    }

    disconnectSigner();
    TxResult result;
    result.is_ok = false;
    result.error = rust::String(message.toStdString());
    result.value = rust::String();
    onSignResult(result);
}

void SpSigning::onSignerCancelled(ReqId req_id) {
    if (req_id != m_sign_req_id) {
        return;
    }
    disconnectSigner();
    m_psbt_result = std::nullopt;
    m_tx_template = std::nullopt;
    m_sign_req_id = 0;
}

void SpSigning::onSignResult(TxResult result) {
    qDebug() << "SpSigning::onSignResult() ok:" << result.is_ok;
    disconnectSigner();

    if (!result.is_ok) {
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        m_sign_req_id = 0;
        AppController::execModal(new catalog::MessageModal(TR("send-signing-failed"), error));
        return;
    }

    m_signed_tx_hex = QString::fromStdString(std::string(result.value.c_str()));
    m_psbt_result = std::nullopt;
    m_sign_req_id = 0;
    m_broadcast_btn->setEnabled(true);
    m_cancel_btn->setEnabled(true);
    updateButtons();
    emit lockedChanged(true);
}

void SpSigning::onBroadcastResult(TxResult result) {
    qDebug() << "SpSigning::onBroadcastResult() ok:" << result.is_ok;
    m_status_label->setVisible(false);
    m_broadcast_btn->setEnabled(true);
    m_cancel_btn->setEnabled(true);

    if (!result.is_ok && m_tx_template.has_value() && !m_signed_tx_hex.isEmpty()) {
        auto &account = m_controller->getAccount();
        if (account.has_value()) {
            account.value()->log_failed_broadcast(m_tx_template.value(),
                                                  m_signed_tx_hex.toStdString());
        }
    }

    dropSigned();

    if (result.is_ok) {
        auto txid = QString::fromStdString(std::string(result.value.c_str()));
        AppController::execModal(
            new catalog::MessageModal(TR("confirm-transaction-title"), TR("confirm-send-success"),
                                      catalog::MessageModal::Tone::Success, txid));
    } else {
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        AppController::execModal(
            new catalog::MessageModal(TR("common-error"), TR("confirm-send-failed").arg(error)));
    }

    // Refresh coin state
    m_controller->pollCoins();

    if (result.is_ok) {
        emit sent();
    }
}
