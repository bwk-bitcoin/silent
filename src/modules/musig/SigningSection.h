#pragma once

#include "account/SendSigning.h"
#include "account/Services.h"
#include "interfaces/backend/types.h"
#include "interfaces/types.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <Qontrol>
#include <optional>
#include <silent.h>

class MusigAccountController;
class QEvent;
struct KeyEntry;

namespace catalog {
class Button;
class ComboBox;
class Label;
} // namespace catalog

namespace musig {

// The signing step of a Multikey account's Send: the spend path under the coins, then
// Sign, which finalizes the crafted transaction into a PSBT (ITransaction::finalize()),
// has the controller adopt it and runs its MuSig2 rounds in the Sign modal, ending on
// Broadcast. While a session runs, Cancel drops it, Export hands its PSBT out as base64
// and Sign opens the modal again at its round.
class SigningSection final : public account::SendSigning {
    Q_OBJECT

public:
    SigningSection(MusigAccountController *controller, const account::Services &services);

    [[nodiscard]] auto actions() -> QWidget * override;
    [[nodiscard]] auto coinsWidget() -> QWidget * override;
    void setTransaction(const std::optional<backend::TransactionTemplate> &tx,
                        const QStringList &outpoints) override;
    // Once a spend path is chosen: the fee depends on it.
    [[nodiscard]] auto estimatesFee() const -> bool override;

public slots:
    // Refreshes the spend path picker from controller->spendLeaves() for `outpoints`,
    // every coin when none.
    void onCoinsSelected(const QStringList &outpoints);
    void onCoinsChanged();
    // The account is back from an endpoint change: spend paths it could not list are
    // listed again.
    void onEndpointStateChanged();
    // A finalize() failure gives its reason as error() of request 0.
    void onTransactionError(backend::RequestId request_id, const QString &message);
    void onSpendLeafChosen(int index);
    void onSignClicked();
    void onPrepared(const QString &psbt_v2_hex, const QString &txid);
    void onSessionLoading();
    void onPrepareFailed(const QString &message);
    void onSent();
    void onCancelClicked();
    void onExportClicked();
    void onSavedPsbtRestored(const SavedPsbtEntry &entry);
    void onSessionCleared();
    void onSaveWarningChanged(const QString &message);

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    [[nodiscard]] static auto leafText(const MusigSpendLeaf &leaf,
                                       const QHash<QString, KeyEntry> &keys) -> QString;
    // The spend path picked in the picker, nullptr when none is.
    [[nodiscard]] auto chosenLeaf() const -> const MusigSpendLeaf *;
    [[nodiscard]] auto defaultLeafIndex(const QHash<QString, KeyEntry> &keys) const -> int;
    [[nodiscard]] auto keyEntries() -> QHash<QString, KeyEntry>;
    // Enables Sign, or says why it cannot start.
    void updateSignState();
    // Runs the Sign modal; once it broadcast, shows Transaction Sent and starts over.
    void openSignModal();
    // The prepared transaction left the step, broadcast or cancelled.
    void closeSession();
    // The unspent coins' outpoints.
    [[nodiscard]] auto unspentOutpoints() const -> QStringList;

private:
    MusigAccountController *m_controller = nullptr;
    account::Services m_services;
    std::optional<backend::TransactionTemplate> m_tx = std::nullopt;
    QStringList m_outpoints;
    bool m_estimates = false;
    // A prepared transaction is held: its MuSig2 session runs.
    bool m_session = false;
    bool m_loading = false;
    // The Sign modal broadcast the transaction.
    bool m_sent = false;
    // The spend path's keys whose signature is in, lowercased: a finalized PSBT no longer
    // carries its rounds, so the Sign modal shows these.

    QWidget *m_leaf_widget = nullptr;
    catalog::Label *m_leaf_title = nullptr;
    catalog::ComboBox *m_leaf_combo = nullptr;
    MusigSpendLeaves m_leaves;
    int m_leaf_index = -1;

    catalog::Label *m_reason_label = nullptr;
    catalog::Label *m_save_warning_label = nullptr;
    QWidget *m_actions = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    catalog::Button *m_export_btn = nullptr;
    catalog::Button *m_sign_btn = nullptr;
    QString m_keys_error;
    QString m_finalize_error;
};

} // namespace musig
