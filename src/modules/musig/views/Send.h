#pragma once

#include "interfaces/types.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <Qontrol>
#include <cstdint>
#include <optional>
#include <silent.h>

class MusigAccountController;
struct KeyEntry;

namespace catalog {
class Button;
class Card;
class ComboBox;
class FieldGroup;
class Input;
class Label;
class Notice;
class Table;
class ValidationMark;
} // namespace catalog

namespace musig::view {

class Send;

// One payment: an address and an amount in BTC.
class PaymentW {
public:
    PaymentW(Send *screen, int id);
    auto widget() -> QWidget *;
    void setRemovable(bool removable);
    void setEditable(bool editable);
    [[nodiscard]] auto address() const -> QString;
    // Sats, nullopt when the text is not a positive amount.
    [[nodiscard]] auto amount() const -> std::optional<uint64_t>;
    void updateValidation();
    void retranslate();

private:
    catalog::FieldGroup *m_fields = nullptr;
    qontrol::Row *m_address_row = nullptr;
    qontrol::Row *m_amount_row = nullptr;
    catalog::Input *m_address_input = nullptr;
    catalog::ValidationMark *m_address_mark = nullptr;
    catalog::Input *m_amount_input = nullptr;
    catalog::ValidationMark *m_amount_mark = nullptr;
    catalog::Button *m_remove_btn = nullptr;
    QWidget *m_widget = nullptr;
};

class Send : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Send(MusigAccountController *ctrl);

public slots:
    void onProcess();
    void onAddOutput();
    void onRemoveOutputClicked();
    void onCoinsUpdated(CoinState state);
    // catalog::Table::rowSelectionChanged, calls onCoinsSelected(selectedIds()).
    void onCoinRowToggled(const QString &row_id, bool selected);
    // Refreshes the spend path picker from controller->spendLeaves() for the inputs.
    void onCoinsSelected(const QStringList &outpoints);
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
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    void initSigningCard();
    void addCoinColumns();
    // nullopt while a payment or the fee rate is not valid.
    auto txTemplate() -> std::optional<TransactionTemplate>;
    void fillCoins();
    [[nodiscard]] static auto leafText(const MusigSpendLeaf &leaf,
                                       const QHash<QString, KeyEntry> &keys) -> QString;
    // The spend path picked in the picker, nullptr when none is.
    [[nodiscard]] auto chosenLeaf() const -> const MusigSpendLeaf *;
    [[nodiscard]] auto defaultLeafIndex(const QHash<QString, KeyEntry> &keys) const -> int;
    [[nodiscard]] auto keyEntries() const -> QHash<QString, KeyEntry>;
    // Enables Prepare, or says why it cannot run.
    void updatePrepareState();
    // Payments, fee, coins and the spend path are locked while a signing session runs.
    void setEditable(bool editable);
    void clearPayments();
    void updateRemovable();
    // The notice title of m_failure, empty for a plain signing error.
    [[nodiscard]] auto failureTitle() const -> QString;

private:
    // What the notice reports, so a language change can retitle it.
    enum class Failure : std::uint8_t { Sign, SessionLost, Finalize };

    MusigAccountController *m_controller = nullptr;
    QWidget *m_main_widget = nullptr;

    int m_next_payment_id = 0;
    QHash<int, PaymentW *> m_payments;
    qontrol::Column *m_payments_column = nullptr;
    catalog::Button *m_add_payment_btn = nullptr;

    catalog::Input *m_fee_input = nullptr;
    catalog::ValidationMark *m_fee_mark = nullptr;
    catalog::Label *m_fee_estimate_label = nullptr;
    // Why the simulation of a valid template failed, empty when it passed.
    QString m_simulation_error;

    catalog::Table *m_coin_table = nullptr;

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

} // namespace musig::view
