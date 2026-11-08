#pragma once

#include "account/Money.h"
#include "account/Services.h"
#include "interfaces/backend/types.h"
#include "interfaces/host.h"
#include "theme/Palette.h"
#include <Qontrol>
#include <cstdint>
#include <optional>
#include <qhash.h>
#include <qscrollarea.h>

namespace catalog {
class Button;
class ComboBox;
} // namespace catalog

namespace catalog {
class Checkbox;
}

namespace catalog {
class Display;
}

namespace catalog {
class Input;
}

namespace catalog {
class Label;
}

namespace catalog {
class Table;
}

namespace catalog {
class Toggle;
}

namespace catalog {
class ScrollArea;
}

namespace catalog {
class Separator;
}

namespace catalog {
class ValidatedInput;
}

namespace catalog {
class Card;
class TxFlowCard;
} // namespace catalog

class QCheckBox;
#include <qstringlist.h>
#include <qtmetamacros.h>
#include <qwidget.h>

namespace account {
class SendSigning;
} // namespace account

namespace account::view {
class Send;

class OutputW {
public:
    OutputW(Send *screen, int id);
    auto widget() -> QWidget *;
    void setDeletable(bool deletable);
    void enableMax(bool max);
    auto isMax() -> bool;
    auto address() -> QString;
    void setAddress(const QString &address);
    auto amount() -> std::optional<uint64_t>;
    auto label() -> QString;
    void syncFiatFromBtc();
    void syncBtcFromFiat();
    auto amountInput() -> QWidget *;
    auto fiatInput() -> QWidget *;
    void updateAddressValidation();
    void updateAmountValidation();
    void setMaxMode(bool max);
    void setMaxAmount(uint64_t sats);
    void setLabel(const QString &label);
    void setFrozenAmount(std::optional<uint64_t> amount);

private:
    catalog::Input *m_address_input = nullptr;
    catalog::Input *m_label_input = nullptr;
    catalog::Input *m_amount_input = nullptr;
    catalog::Input *m_amount_fiat_input = nullptr;
    catalog::Label *m_fiat_code = nullptr;
    QWidget *m_fiat_group = nullptr;
    catalog::ValidatedInput *m_address_field = nullptr;
    catalog::ValidatedInput *m_amount_field = nullptr;
    catalog::Button *m_delete_btn = nullptr;
    catalog::Button *m_scan_btn = nullptr;
    catalog::Checkbox *m_max = nullptr;
    catalog::Label *m_max_label = nullptr;
    QWidget *m_widget = nullptr;
    std::optional<uint64_t> m_frozen_amount;
    std::optional<uint64_t> m_parsed_amount;
    QString m_parsed_text;
};

// Payments, coins and fee, the transaction they form, then the account's signing
// step (account::SendSigning, which Send takes) where the kinds of account differ. The
// transaction is crafted from a template: by ITransaction::craft() over coins picked by
// hand, by an explicit ITransaction::selectCoins() otherwise.
class Send : public qontrol::Screen {
    Q_OBJECT
public:
    Send(const Services &services, SendSigning *signing);
    // Shows `transaction`, spending `inputs`, frozen: the signing step holds it again.
    void restoreTransaction(const backend::TransactionTemplate &transaction,
                            const QList<backend::Coin> &inputs, const QString &warning);

public slots:
    void onOutputSetMax(int id);
    void onDeleteOutput(int id);
    void onAddOutput();
    void onOutputDeleteClicked();
    void onScanAddressClicked();
    void onOutputMaxToggled();
    void onClearOutputs();
    void onClearInputs();
    void onFeeToggled();
    void onFeeOverrideToggled();
    void onCoinToggled();
    void onCoinSelectionChanged(const QString &row_id, bool selected);
    void onAutoSelectionToggled();
    void onUpdateInputsTotal();
    void onUpdateOutputValidations();
    void onUpdateFeeValidation();
    void onUpdateSelectedCoinsFromSimulation();
    void onProcess();
    void onFiatEdited();
    void onSelectedPriceChanged();
    void onSelectedFeesChanged();
    void onFeeEdited();
    void onFeeModeActivated(int index);
    void onFeeTierActivated(int index);
    void onCoinsChanged();
    void onCoinsSelected(backend::RequestId request_id, const backend::CraftResult &result);
    void onSelectionError(backend::RequestId request_id, const QString &message);
    // The signing step took or let go of a transaction.
    void onSigningLocked(bool locked);
    // The signing step broadcast the transaction.
    void onSigningSent();
    void onSavedWarningChanged(const QString &message);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void resizeEvent(QResizeEvent *event) override;
    auto outputsView() -> QWidget *;
    auto inputsView() -> QWidget *;
    auto transactionView() -> QWidget *;
    // Recompute the flow diagram + summary values and push them into the existing
    // widgets in place, so the transaction view tracks edits live without
    // rebuilding/re-parenting anything (no input focus loss).
    void updateTransactionFlow();
    [[nodiscard]] auto editableAmounts() -> money::Amounts;
    auto txTemplate() -> std::optional<backend::TransactionTemplate>;
    // The unspent coins.
    [[nodiscard]] auto availableCoins() const -> QList<backend::Coin>;
    // Shows the crafted `result` of the template the payments, fee and coins form.
    void applyCraft(const backend::CraftResult &result);
    // The payments, fee and coins form no transaction now: `warning` tells why, when set.
    void clearTransaction(const QString &warning);
    // The coins the transaction spends: the ones picked by hand, or the auto selection.
    [[nodiscard]] auto spentOutpoints() const -> QStringList;
    // Payments, fee and coins are frozen while the signing step holds a transaction.
    void applyLocked();
    auto output() -> QWidget *;
    void updateCoinCheckboxes();
    void updateCoinRows();
    void applyInputsMode();
    void updateInputsTitle();
    void initializeFees();

private:
    // A complete crafted transaction and what it spends.
    struct Preview {
        backend::TransactionTemplate transaction;
        // The value of each input, in order.
        QList<quint64> inputValues;
        money::Amounts amounts;
    };

    static constexpr int SPLIT_WIDTH = 1569;
    Services m_services;
    account::SendSigning *m_signing = nullptr;
    bool m_locked = false;
    bool m_restored = false;
    bool m_stacked = true;
    int m_output_id = 0;
    QHash<int, OutputW *> m_outputs;
    qontrol::Column *m_outputs_column = nullptr;
    qontrol::Column *m_inputs_column = nullptr;

    QWidget *m_main_widget = nullptr;
    QWidget *m_outputs_frame = nullptr;
    QWidget *m_inputs_frame = nullptr;
    QWidget *m_transaction_frame = nullptr;
    // Transaction-view widgets updated in place by updateTransactionFlow().
    catalog::TxFlowCard *m_flow_card = nullptr;
    // Transaction summary values, split into a BTC column and a fiat column.
    catalog::Label *m_sending_btc = nullptr;
    catalog::Label *m_sending_fiat = nullptr;
    catalog::Label *m_fee_btc = nullptr;
    catalog::Label *m_fee_fiat = nullptr;
    catalog::Label *m_total_btc = nullptr;
    catalog::Label *m_total_fiat = nullptr;
    catalog::Label *m_change_label = nullptr;
    catalog::Label *m_change_btc = nullptr;
    catalog::Label *m_change_fiat = nullptr;

    catalog::Toggle *m_fee_toggle = nullptr;
    std::optional<money::Fee> m_parsed_fee;
    catalog::ComboBox *m_fee_mode_picker = nullptr;
    catalog::ComboBox *m_fee_tier_picker = nullptr;
    FeeMode m_fee_mode = FeeMode::Manual;
    FeeTier m_fee_tier = FeeTier::Medium;
    catalog::Input *m_fee_value_input = nullptr;
    catalog::Label *m_fee_label = nullptr;
    catalog::Label *m_fee_rate_label = nullptr;
    catalog::ValidatedInput *m_fee_field = nullptr;
    // The fee rate label over its toggle, value and unit, hidden with them.
    QWidget *m_fee_rate_group = nullptr;

    catalog::Label *m_warning_label = nullptr;
    catalog::Label *m_inputs_title_label = nullptr;

    catalog::Button *m_add_output_btn = nullptr;
    catalog::ScrollArea *m_coins_scroll = nullptr;
    catalog::Table *m_coin_table = nullptr;

    // "Remaining to select" and "Selected" on one row, split by a vertical rule.
    QWidget *m_inputs_amounts_row = nullptr;
    catalog::Label *m_inputs_min_value = nullptr;   // remaining to select
    catalog::Label *m_inputs_total_value = nullptr; // total selected
    catalog::Checkbox *m_auto_coin_selection = nullptr;
    // The auto selection's total, in place of the coin table when stacked.
    catalog::Card *m_auto_summary = nullptr;
    catalog::Label *m_auto_amount = nullptr;
    catalog::Label *m_auto_count = nullptr;

    catalog::Button *m_clear_outputs_btn = nullptr;
    catalog::Button *m_clear_inputs_btn = nullptr;

    uint64_t m_estimated_fee = 0;
    std::optional<Preview> m_preview;
    // The coin selection in flight, 0 when none: an older answer is stale.
    backend::RequestId m_selection_request = 0;

    QList<backend::Coin> m_selected_coins{};
    QStringList m_auto_selected_outpoints;
    backend::Balance m_last_balance{};
};

} // namespace account::view
