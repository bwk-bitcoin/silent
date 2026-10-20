#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/types.h"

#include <QSet>
#include <QString>
#include <cstdint>
#include <memory>
#include <silent.h>

class MusigAccountController;
class QEvent;

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
class WizardSteps;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig {

// Sign on a Multikey Send: the MuSig2 rounds of the prepared transaction. Nonces, then
// Partial Signatures, which ends on Broadcast. Each round lists every key of the
// wallet: the spend path's signers first, those that can sign ahead of those that
// cannot (no connected device, no route), then the keys the path does not use, greyed
// out. A click on a signer signs with its key alone. A single-key path (a timelocked
// one) signs once, with no rounds. A key on its connected USB
// device signs in place, its row "On Device…" until the device answers; any other
// route runs its own window over this modal, which stays under it. A round cannot go back (a
// nonce is spent), so there is no Back; closing keeps the session, Sign on the Send
// panel opens it again.
class SignModal final : public catalog::Modal {
    Q_OBJECT

public:
    explicit SignModal(MusigAccountController *controller);

public slots:
    void onCosignerClicked();
    void onContinueClicked();
    void onBroadcastClicked();
    void onRoundChanged(const MusigRoundStatus &status);
    void onSignerAnswered(const QString &fingerprint);
    void onTransactionReady(const QString &tx_hex);
    void onSigningFailed(const QString &message, plugin::SignError kind);
    void onFinalizeFailed(const QString &message);
    void onBroadcastDone(const TxResult &result);
    void onPlanReady();
    void onDevicesChanged();
    void onRetryClicked();

protected:
    // What the notice reports, so a language change can retitle it.
    enum class Failure : std::uint8_t { None, Sign, SessionLost, Finalize };

    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    // Rebuilds the rows, the step and the action from the controller's round.
    void refresh();
    void requestPlan();
    // The same from `status`.
    void render(const MusigRoundStatus &status);
    void showFailure(Failure failure, const QString &text);
    [[nodiscard]] auto failureTitle() const -> QString;
    // Whether the key `fingerprint` signs on its connected USB device.
    [[nodiscard]] auto onDevice(const QString &fingerprint) const -> bool;

private:
    MusigAccountController *m_controller = nullptr;
    catalog::WizardSteps *m_steps = nullptr;
    catalog::Notice *m_notice = nullptr;
    catalog::Label *m_lead = nullptr;
    qontrol::Column *m_rows = nullptr;
    catalog::Button *m_continue_btn = nullptr;
    catalog::Button *m_broadcast_btn = nullptr;
    catalog::Button *m_retry_btn = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    // The partial signatures step is shown: Continue was clicked, or the modal opened
    // on a session past its nonces.
    bool m_partial = false;
    Failure m_failure = Failure::None;
    // The keys of a single-key path that signed, lowercased.
    QSet<QString> m_signed;
    std::shared_ptr<SigningPlan> m_plan;
    QString m_plan_psbt;
    bool m_plan_loading = false;
    bool m_plan_ready = false;
    bool m_plan_dirty = false;
};

} // namespace musig
