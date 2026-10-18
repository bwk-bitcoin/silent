#pragma once

#include "account/SendSigning.h"
#include "interfaces/types.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <Qontrol>
#include <cstdint>
#include <optional>
#include <silent.h>

class MusigAccountController;
class QEvent;
struct KeyEntry;

namespace catalog {
class Button;
class Card;
class ComboBox;
class Label;
class Notice;
} // namespace catalog

namespace musig {

// The signing step of a Multikey account's Send: the spend path, Prepare, then the
// MuSig2 rounds one keys()->sign() at a time, and Broadcast once finalized.
class SigningSection final : public account::SendSigning {
    Q_OBJECT

public:
    explicit SigningSection(MusigAccountController *controller);

    [[nodiscard]] auto actions() -> QWidget * override;
    void setTransaction(const std::optional<TransactionTemplate> &tx,
                        const QStringList &outpoints) override;
    // Once a spend path is chosen: the fee depends on it.
    [[nodiscard]] auto estimatesFee() const -> bool override;

public slots:
    // Refreshes the spend path picker from controller->spendLeaves() for `outpoints`,
    // every coin when none.
    void onCoinsSelected(const QStringList &outpoints);
    void onCoinsUpdated(CoinState state);
    void onSpendLeafChosen(int index);
    void onPrepareClicked();
    void onPrepared(const QString &psbt_v2_hex, const QString &txid);
    void onPrepareFailed(const QString &message);
    void onRoundChanged(const MusigRoundStatus &status);
    void onSignRoundClicked();
    void onSigningFailed(const QString &message, plugin::SignError kind);
    void onSigningCancelled();
    void onTransactionReady(const QString &tx_hex);
    void onFinalizeFailed(const QString &message);
    void onBroadcastClicked();
    void onBroadcastDone(const TxResult &result);
    void onCancelClicked();

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    void initSigningCard();
    [[nodiscard]] static auto leafText(const MusigSpendLeaf &leaf,
                                       const QHash<QString, KeyEntry> &keys) -> QString;
    // The spend path picked in the picker, nullptr when none is.
    [[nodiscard]] auto chosenLeaf() const -> const MusigSpendLeaf *;
    [[nodiscard]] auto defaultLeafIndex(const QHash<QString, KeyEntry> &keys) const -> int;
    [[nodiscard]] auto keyEntries() const -> QHash<QString, KeyEntry>;
    // Enables Prepare, or says why it cannot run.
    void updatePrepareState();
    // The prepared transaction left the step, broadcast or cancelled.
    void closeSession();
    // The notice title of m_failure, empty for a plain signing error.
    [[nodiscard]] auto failureTitle() const -> QString;

private:
    // What the notice reports, so a language change can retitle it.
    enum class Failure : std::uint8_t { Sign, SessionLost, Finalize };

    MusigAccountController *m_controller = nullptr;
    std::optional<TransactionTemplate> m_tx = std::nullopt;
    QStringList m_outpoints;
    bool m_estimates = false;

    catalog::Label *m_leaf_title = nullptr;
    catalog::ComboBox *m_leaf_combo = nullptr;
    MusigSpendLeaves m_leaves;
    int m_leaf_index = -1;

    catalog::Button *m_prepare_btn = nullptr;
    catalog::Label *m_prepare_reason_label = nullptr;

    catalog::Card *m_signing_card = nullptr;
    catalog::Label *m_txid_caption = nullptr;
    catalog::Label *m_txid_label = nullptr;
    catalog::Label *m_round_label = nullptr;
    qontrol::Column *m_cosigners_column = nullptr;
    QList<QWidget *> m_cosigner_rows;
    catalog::Notice *m_notice = nullptr;
    Failure m_failure = Failure::Sign;
    catalog::Button *m_cancel_btn = nullptr;
    catalog::Button *m_sign_btn = nullptr;
    catalog::Button *m_broadcast_btn = nullptr;
};

} // namespace musig
