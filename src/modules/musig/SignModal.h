#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/types.h"

#include <QString>
#include <cstdint>
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
// Partial Signatures, which ends on Broadcast. Each round lists the cosigners of the
// spend path; a click on one signs with its key alone, the modal hidden while its
// route runs. A round cannot go back (a nonce is spent), so there is no Back; closing
// keeps the session, Sign on the Send panel opens it again.
class SignModal final : public catalog::Modal {
    Q_OBJECT

public:
    explicit SignModal(MusigAccountController *controller);
    auto exec() -> int override;

public slots:
    void onCosignerClicked();
    void onContinueClicked();
    void onBroadcastClicked();
    void onRoundChanged(const MusigRoundStatus &status);
    void onTransactionReady(const QString &tx_hex);
    void onSigningFailed(const QString &message, plugin::SignError kind);
    void onFinalizeFailed(const QString &message);
    void onBroadcastDone(const TxResult &result);

protected:
    // What the notice reports, so a language change can retitle it.
    enum class Failure : std::uint8_t { None, Sign, SessionLost, Finalize };

    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    // Signs with the cosigner clicked, the modal hidden while the flow runs.
    void runFlow();
    // Rebuilds the rows, the step and the action from the controller's round.
    void refresh();
    // The same from `status`.
    void render(const MusigRoundStatus &status);
    void showFailure(Failure failure, const QString &text);
    [[nodiscard]] auto failureTitle() const -> QString;

private:
    MusigAccountController *m_controller = nullptr;
    catalog::WizardSteps *m_steps = nullptr;
    catalog::Notice *m_notice = nullptr;
    catalog::Label *m_lead = nullptr;
    qontrol::Column *m_rows = nullptr;
    catalog::Button *m_continue_btn = nullptr;
    catalog::Button *m_broadcast_btn = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    // The partial signatures step is shown: Continue was clicked, or the modal opened
    // on a session past its nonces.
    bool m_partial = false;
    Failure m_failure = Failure::None;
    QString m_flow_fingerprint;
};

} // namespace musig
