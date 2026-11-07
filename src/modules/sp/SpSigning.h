#pragma once

#include "account/SendSigning.h"
#include "account/Services.h"
#include "interfaces/backend/types.h"
#include "interfaces/types.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <optional>
#include <silent.h>

class AccountController;

namespace catalog {
class Button;
class Label;
} // namespace catalog

// The signing step of an sp account's Send: Sign finalizes the crafted transaction into
// a PSBT (ITransaction::finalize()) and has the keys module sign it at once, then
// Broadcast sends it (IBlockchain::broadcast()), or Cancel lets it go.
class SpSigning final : public account::SendSigning {
    Q_OBJECT

public:
    SpSigning(AccountController *controller, const account::Services &services);

    [[nodiscard]] auto actions() -> QWidget * override;
    [[nodiscard]] auto coinsWidget() -> QWidget * override;
    void setTransaction(const std::optional<backend::TransactionTemplate> &tx,
                        const QStringList &outpoints) override;
    [[nodiscard]] auto estimatesFee() const -> bool override;

signals:
    // Emitted from the worker threads; queued to the GUI thread receivers.
    void validationReady(PsbtValidation result);
    void signReady(TxResult result);

public slots:
    void onSignClicked();
    void onBroadcastClicked();
    void onCancelClicked();
    void onValidationResult(PsbtValidation result);
    void onSignerSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onSignerError(std::optional<ReqId> req_id, const QString &message);
    void onSignerCancelled(ReqId req_id);
    void onSignResult(TxResult result);
    void onBroadcastFinished(backend::RequestId request_id, const backend::BroadcastResult &result);
    void onBroadcastError(backend::RequestId request_id, const QString &message);
    // A finalize() failure gives its reason as error() of request 0.
    void onTransactionError(backend::RequestId request_id, const QString &message);

protected:
    void init();
    void doConnect();
    void view();
    // Warns about address reuse and asks to go on. false when the user cancelled
    // (the transaction is dropped).
    static auto showReuseWarning(PsbtValidation result) -> bool;
    // Hands the PSBT to the keys module, once it is reachable.
    void showSignerStep();
    // Stops listening to the keys module once the signing request has its answer.
    void disconnectSigner() const;
    // Sign before signing, the signed pill, Cancel and Broadcast after.
    void updateButtons();
    // Lets go of the prepared or signed transaction, and Send of its payments.
    void dropTransaction();
    // A prepare failure as the user reads it: what failed, then why when known.
    [[nodiscard]] static auto failureText(const QString &reason) -> QString;
    // The broadcast answered: `error` is empty once the network accepted it.
    void finishBroadcast(const QString &error);

private:
    AccountController *m_controller = nullptr;
    account::Services m_services;
    QWidget *m_actions = nullptr;
    QWidget *m_signed_pill = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    catalog::Button *m_sign_btn = nullptr;
    catalog::Button *m_broadcast_btn = nullptr;
    catalog::Label *m_status_label = nullptr;
    // The transaction Send forms now.
    std::optional<backend::TransactionTemplate> m_tx = std::nullopt;
    // The transaction being signed or held signed, and its unsigned PSBTv2 until it is
    // signed.
    std::optional<backend::TransactionTemplate> m_tx_template = std::nullopt;
    QByteArray m_psbt;
    QString m_signed_tx_hex;
    ReqId m_sign_req_id = 0;
    backend::RequestId m_broadcast_req = 0;
    QString m_finalize_error;
};
