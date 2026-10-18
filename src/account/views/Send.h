#pragma once

#include "theme/Palette.h"
#include <Qontrol>
#include <cstdint>
#include <optional>
#include <qhash.h>
#include <qscrollarea.h>

namespace catalog {
class Button;
}

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
class ValidationMark;
}

namespace catalog {
class TxFlow;
}

namespace catalog {
class Card;
}

class QCheckBox;
#include <qstringlist.h>
#include <qtmetamacros.h>
#include <qwidget.h>
#include <silent.h>

class AccountBackend;

namespace account {
class SendSigning;
} // namespace account

namespace account::view {
class Send;

class InputW {
public:
    InputW(const RustCoin &coin);
    auto widget() -> QWidget *;
    static const int TYPE_WIDTH = resolve(Size::XXS);
    static const int OUTPOINT_WIDTH = resolve(Size::M);
    static const int LABEL_WIDTH = resolve(Size::XL);
    static const int VALUE_WIDTH = resolve(Size::S);

private:
    QWidget *m_widget = nullptr;
};

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
    void syncUsdFromBtc();
    void syncBtcFromUsd();
    auto amountInput() -> QWidget *;
    auto usdInput() -> QWidget *;
    void updateAddressValidation();
    void updateAmountValidation();
    void setMaxMode(bool max);
    void setMaxAmount(uint64_t sats);

private:
    catalog::Input *m_address_input = nullptr;
    catalog::Input *m_label_input = nullptr;
    catalog::Input *m_amount_input = nullptr;
    catalog::Input *m_amount_usd_input = nullptr;
    catalog::ValidationMark *m_address_indicator = nullptr;
    catalog::ValidationMark *m_amount_indicator = nullptr;
    catalog::Button *m_delete_btn = nullptr;
    catalog::Button *m_scan_btn = nullptr;
    catalog::Checkbox *m_max = nullptr;
    catalog::Label *m_max_label = nullptr;
    QWidget *m_widget = nullptr;
};

// Payments, coins and fee, the transaction they form, then the account's signing
// step (account::SendSigning) where the kinds of account differ.
class Send : public qontrol::Screen {
    Q_OBJECT
public:
    explicit Send(AccountBackend *backend);

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
    void onCoinToggled();
    void onCoinSelectionChanged(const QString &row_id, bool selected);
    void onAutoSelectionToggled();
    void onUpdateInputsTotal();
    void onUpdateOutputValidations();
    void onUpdateFeeValidation();
    void onUpdateSelectedCoinsFromSimulation();
    void onProcess();
    void onCoinsUpdated(CoinState state);
    // The signing step took or let go of a transaction.
    void onSigningLocked(bool locked);
    // The signing step broadcast the transaction.
    void onSigningSent();

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
    auto txTemplate() -> std::optional<TransactionTemplate>;
    // The coins the transaction spends: the ones picked by hand, or the auto selection.
    [[nodiscard]] auto spentOutpoints() const -> QStringList;
    // Payments, fee and coins are frozen while the signing step holds a transaction.
    void applyLocked();
    auto output() -> QWidget *;
    void updateCoinCheckboxes();
    void applyInputsMode();
    void updateInputsTitle();

private:
    static constexpr int SPLIT_WIDTH = 1569;
    AccountBackend *m_backend = nullptr;
    account::SendSigning *m_signing = nullptr;
    bool m_locked = false;
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
    catalog::TxFlow *m_tx_flow = nullptr;
    catalog::Card *m_flow_card = nullptr;
    catalog::Label *m_flow_inputs_eyebrow = nullptr;
    catalog::Label *m_flow_outputs_eyebrow = nullptr;
    QWidget *m_legend_change = nullptr;
    QWidget *m_legend_fee = nullptr;
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
    catalog::Input *m_fee_value_input = nullptr;
    catalog::Label *m_fee_label = nullptr;
    catalog::Label *m_fee_rate_label = nullptr;
    catalog::ValidationMark *m_fee_indicator = nullptr;

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

    catalog::Label *m_fee_estimate_label = nullptr;
    uint64_t m_estimated_fee = 0;

    QList<RustCoin> m_selected_coins{};
    QStringList m_auto_selected_outpoints;
    CoinState m_last_coin_state{};
};

} // namespace account::view
