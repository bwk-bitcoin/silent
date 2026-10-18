#pragma once

#include <QStringList>
#include <QWidget>
#include <optional>
#include <silent.h>

namespace account {

// The signing step of Send, the part of it each kind of account does its own way: an
// sp account signs then broadcasts in one go, a Multikey account picks a spend path
// and runs its MuSig2 rounds. Send places the step's own widget under its fee row,
// actions() at the end of that row, and coinsWidget() under its coins.
class SendSigning : public QWidget {
    Q_OBJECT

public:
    using QWidget::QWidget;
    ~SendSigning() override = default;

    // What goes at the end of Send's fee row: the step's primary buttons.
    [[nodiscard]] virtual auto actions() -> QWidget * = 0;
    // What goes under Send's coins, part of picking what to spend; nullptr for none.
    [[nodiscard]] virtual auto coinsWidget() -> QWidget * = 0;
    // The transaction the payments, fee and coins form now, nullopt while they form
    // none. `outpoints` are the coins it spends: the ones picked by hand, or the ones
    // the auto selection took.
    virtual void setTransaction(const std::optional<TransactionTemplate> &tx,
                                const QStringList &outpoints) = 0;
    // Whether Send can show a fee estimate yet.
    [[nodiscard]] virtual auto estimatesFee() const -> bool = 0;

signals:
    // Send freezes its payments, fee and coins while the step holds a transaction.
    void lockedChanged(bool locked);
    // estimatesFee() changed.
    void estimateChanged();
    // The transaction was broadcast: Send starts over with empty payments and coins.
    void sent();
};

} // namespace account
