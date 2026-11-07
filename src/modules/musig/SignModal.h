#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/backend/types.h"
#include "interfaces/types.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <silent.h>

class MusigAccountController;
class QEvent;
struct KeyEntry;

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
class SelectRow;
class WizardSteps;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace backend {
class IBlockchain;
} // namespace backend

namespace musig {

class ConfirmSigningReset final : public catalog::Modal {
    Q_OBJECT

public:
    ConfirmSigningReset();

signals:
    void confirmed();

public slots:
    void onConfirmClicked();

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    catalog::Label *m_message_label = nullptr;
    catalog::Button *m_confirm_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

// Sign on a Multikey Send: the MuSig2 rounds of the prepared transaction. Nonces, then
// Partial Signatures, which ends on Broadcast. Each round lists every key of the
// wallet: the spend path's signers first, those that can sign ahead of those that
// cannot (no connected device, no route), then the keys the path does not use, greyed
// out. A click on a signer signs with its key alone. A single-key path (a timelocked
// one) signs once, with no rounds. A key on its connected USB
// device signs in place, its row "On Device…" until the device answers; any other
// route runs its own window over this modal, which stays under it. A round cannot go back (a
// nonce is spent), so there is no Back; closing keeps the session, Sign on the Send
// panel opens it again. Finalized, it lists the spend path's keys with the signatures
// known to be in, and Broadcast, which sends it through `blockchain`.
class SignModal final : public catalog::Modal {
    Q_OBJECT

public:
    SignModal(MusigAccountController *controller, backend::IBlockchain *blockchain);

signals:
    // The network accepted the broadcast transaction; the modal closes.
    void sent();

public slots:
    void onCosignerClicked();
    void onContinueClicked();
    void onBroadcastClicked();
    void onRoundChanged(const MusigRoundStatus &status);
    void onTransactionReady(const QString &tx_hex);
    void onSigningFailed(const QString &message, plugin::SignError kind);
    void onFinalizeFailed(const QString &message);
    void onBroadcastFinished(backend::RequestId request_id, const backend::BroadcastResult &result);
    void onBroadcastError(backend::RequestId request_id, const QString &message);
    void onPlanReady(uint64_t req_id, const SigningPlan &plan);
    void onDevicesChanged();
    void onRetryClicked();
    void onResetConfirmed();
    void onSigningReset();
    void onSaveWarningChanged(const QString &message);

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
    void renderFinalized();
    auto refreshKeys() -> bool;
    [[nodiscard]] auto keyEntries() const -> QHash<QString, KeyEntry>;
    // The wallet's keys the spend path `signers` does not use, greyed out.
    [[nodiscard]] auto otherRows(const QStringList &signers,
                                 const QHash<QString, KeyEntry> &entries) const
        -> QList<catalog::SelectRow *>;
    // Connected devices whose key is not one of the wallet's, greyed out.
    [[nodiscard]] auto strangerRows(const QHash<QString, KeyEntry> &entries) const
        -> QList<catalog::SelectRow *>;
    void showFailure(Failure failure, const QString &text);
    [[nodiscard]] auto failureTitle() const -> QString;
    // Whether the key `fingerprint` signs on its connected USB device.
    [[nodiscard]] auto onDevice(const QString &fingerprint) const -> bool;
    // Whether every signer that still owes a contribution already holds a usable
    // cached session, so the nonce step has nothing to prepare.
    [[nodiscard]] auto everyPendingCached() const -> bool;

private:
    MusigAccountController *m_controller = nullptr;
    QPointer<backend::IBlockchain> m_blockchain;
    backend::RequestId m_broadcast_req = 0;
    catalog::WizardSteps *m_steps = nullptr;
    catalog::Notice *m_notice = nullptr;
    catalog::Label *m_save_warning_label = nullptr;
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
    std::shared_ptr<SigningPlan> m_plan;
    QString m_plan_psbt;
    uint64_t m_plan_req_id = 0;
    bool m_plan_loading = false;
    bool m_plan_ready = false;
    bool m_plan_dirty = false;
    QHash<QString, KeyEntry> m_key_entries;
};

} // namespace musig
