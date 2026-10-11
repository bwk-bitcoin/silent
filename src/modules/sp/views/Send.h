#pragma once

#include "interfaces/types.h"
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

class AccountController;

namespace view {
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
    catalog::Checkbox *m_max = nullptr;
    catalog::Label *m_max_label = nullptr;
    QWidget *m_widget = nullptr;
};

class Send : public qontrol::Screen {
    Q_OBJECT
public:
    Send(AccountController *ctrl);

signals:
    void validationReady(PsbtValidation result);
    void signReady(TxResult result);
    void broadcastReady(TxResult result);

public slots:
    void onOutputSetMax(int id);
    void onDeleteOutput(int id);
    void onAddOutput();
    void onOutputDeleteClicked();
    void onOutputMaxToggled();
    void onClearOutputs();
    void onClearInputs();
    void onFeeToggled();
    void onSetBroadcastable(bool broadcastable);
    void onCoinToggled();
    void onCoinSelectionChanged(const QString &row_id, bool selected);
    void onAutoSelectionToggled();
    void onUpdateInputsTotal();
    void onUpdateOutputValidations();
    void onUpdateFeeValidation();
    void onUpdateSelectedCoinsFromSimulation();
    void onSetSpendable(bool spendable);
    void onProcess();
    void onSendTransaction();
    void onValidationResult(PsbtValidation result);
    void onSendConfirmed();
    void onBroadcastConfirmed();
    void onSignResult(TxResult result);
    void onSignerSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onSignerError(std::optional<ReqId> req_id, const QString &message);
    void onSignerCancelled(ReqId req_id);
    void onBroadcastResult(TxResult result);
    void onCoinsUpdated(CoinState state);

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
    // Warn about address reuse and ask the user to confirm. Returns true to
    // proceed, false if the user cancelled (transaction state is reset).
    auto showReuseWarning(PsbtValidation result) -> bool;
    // Guards the keys module is reachable, then hands the PSBT over to it.
    void showSignerStep();
    // Stops listening to the keys module once the signing request has its answer.
    void disconnectSigner() const;
    auto output() -> QWidget *;
    void updateCoinCheckboxes();
    void applyInputsMode();
    void updateInputsTitle();
    // Sign and Broadcast share one enabled state; only one shows at a time.
    void setSendEnabled(bool enabled);

private:
    static constexpr int SPLIT_WIDTH = 1569;
    AccountController *m_controller = nullptr;
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
    catalog::Button *m_sign_btn = nullptr;
    catalog::Button *m_broadcast_btn = nullptr;

    catalog::Label *m_fee_estimate_label = nullptr;
    uint64_t m_estimated_fee = 0;

    bool m_broadcastable = false;
    std::optional<rust::Box<PsbtResult>> m_psbt_result = std::nullopt;
    std::optional<TransactionTemplate> m_tx_template = std::nullopt;
    QString m_signed_tx_hex;
    ReqId m_sign_req_id = 0;
    /// SP change of m_signed_tx_hex, kept for broadcast once the psbt is dropped.
    QList<RustCoin> m_selected_coins{};
    QStringList m_auto_selected_outpoints;
    CoinState m_last_coin_state{};
};

} // namespace view
