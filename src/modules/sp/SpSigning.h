#pragma once

#include "account/SendSigning.h"
#include "interfaces/types.h"

#include <QString>
#include <QStringList>
#include <optional>
#include <silent.h>

class AccountController;

namespace catalog {
class Button;
class Label;
} // namespace catalog

// The signing step of an sp account's Send: Sign has the keys module sign the whole
// PSBT at once, then Broadcast sends it, or Cancel lets it go.
class SpSigning final : public account::SendSigning {
    Q_OBJECT

public:
    explicit SpSigning(AccountController *controller);

    [[nodiscard]] auto actions() -> QWidget * override;
    [[nodiscard]] auto coinsWidget() -> QWidget * override;
    void setTransaction(const std::optional<TransactionTemplate> &tx,
                        const QStringList &outpoints) override;
    [[nodiscard]] auto estimatesFee() const -> bool override;

signals:
    // Emitted from the worker threads; queued to the GUI thread receivers.
    void validationReady(PsbtValidation result);
    void signReady(TxResult result);
    void broadcastReady(TxResult result);

public slots:
    void onSignClicked();
    void onBroadcastClicked();
    void onCancelClicked();
    void onValidationResult(PsbtValidation result);
    void onSignerSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onSignerError(std::optional<ReqId> req_id, const QString &message);
    void onSignerCancelled(ReqId req_id);
    void onSignResult(TxResult result);
    void onBroadcastResult(TxResult result);

protected:
    void init();
    void doConnect();
    void view();
    // Warns about address reuse and asks to go on. false when the user cancelled
    // (the transaction is dropped).
    auto showReuseWarning(PsbtValidation result) -> bool;
    // Hands the PSBT to the keys module, once it is reachable.
    void showSignerStep();
    // Stops listening to the keys module once the signing request has its answer.
    void disconnectSigner() const;
    // Sign before signing, the signed pill, Cancel and Broadcast after.
    void updateButtons();
    // Lets go of the signed transaction, and Send of its payments.
    void dropSigned();

private:
    AccountController *m_controller = nullptr;
    QWidget *m_actions = nullptr;
    QWidget *m_signed_pill = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    catalog::Button *m_sign_btn = nullptr;
    catalog::Button *m_broadcast_btn = nullptr;
    catalog::Label *m_status_label = nullptr;
    // The transaction Send forms now.
    std::optional<TransactionTemplate> m_tx = std::nullopt;
    // The transaction being signed or held signed, and its PSBT until it is signed.
    std::optional<TransactionTemplate> m_tx_template = std::nullopt;
    std::optional<rust::Box<PsbtResult>> m_psbt_result = std::nullopt;
    QString m_signed_tx_hex;
    ReqId m_sign_req_id = 0;
};
