#include "Coins.h"
#include "AccountController.h"
#include "catalog/BalanceHeader.h"
#include "catalog/Table.h"
#include "i18n/Tr.h"
#include "views/utils.h"
#include <Qontrol>
#include <common.h>
#include <cstdint>
#include <optional>
#include <utility>

namespace view {

using catalog::DisplayRole;

Coins::Coins(AccountController *ctrl) {
    m_controller = ctrl;
    this->init();
    this->doConnect();
    this->view();
}

void Coins::init() {
}

void Coins::onRecvPayload(const CoinState &state) {
    m_state = state;
    this->view();
    emit coinsUpdated();
}

void Coins::doConnect() {
    auto *ctrl = m_controller;
    connect(ctrl, &AccountController::updateCoins, this, &Coins::onRecvPayload, qontrol::UNIQUE);
}

void Coins::view() {
    if (m_controller == nullptr) {
        return;
    }
    m_coins = m_controller->getCoins();

    m_balance = new catalog::BalanceHeader;
    m_balance->setConfirmed(toBitcoin(m_state.confirmed_balance, false));
    m_balance->setUnconfirmed(m_state.unconfirmed_balance);
    m_balance->setCoinCount(m_state.confirmed_count + m_state.unconfirmed_count);

    m_table = new catalog::Table;
    m_table->addColumn({.title = TR("coins-type"), .width = TYPE_W});
    m_table->addColumn({.title = TR("coins-height"), .width = HEIGHT_W});
    m_table->addColumn(
        {.title = TR("coins-outpoint"), .width = OUTPOINT_W, .role = DisplayRole::Outpoint});
    m_table->addColumn(
        {.title = TR("coins-label"), .width = LABEL_W, .grow = true, .editable = true});
    m_table->addColumn({.title = TR("coins-value"),
                        .width = VALUE_W,
                        .alignment = Qt::AlignRight,
                        .role = DisplayRole::Amount});
    connect(m_table, &catalog::Table::cellEdited, this, &Coins::onCellEdited, qontrol::UNIQUE);

    for (const auto &coin : m_coins) {
        QString typeStr = QString::fromUtf8(coin.account_type.data(), coin.account_type.size());

        QString heightStr;
        if (!coin.spent && coin.height > 0) {
            heightStr = QString::number(coin.height);
        } else if (coin.spent) {
            heightStr = TR("coins-spent");
        } else {
            heightStr = TR("coins-unconfirmed-no-colon");
        }
        QString outpoint = QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size());
        QString label = QString::fromUtf8(coin.label.data(), coin.label.size());

        m_table->addRow(outpoint, {typeStr, heightStr, shortenOutpoint(outpoint), label,
                                   toBitcoin(coin.value)});
    }

    auto *mainLayout = (new qontrol::Column(this))->push(m_balance)->push(m_table);

    auto *boxed = dashboard(TR("coins-title"), mainLayout);
    setScreenContent(this, m_main_widget, boxed);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto Coins::getCoins() -> std::optional<QList<RustCoin>> {
    auto coins = QList<RustCoin>();
    for (const auto &coin : m_coins) {
        coins.append(coin);
    }
    if (coins.isEmpty()) {
        return std::nullopt;
    }
    return std::make_optional(coins);
}

void Coins::onCellEdited(const QString &outpoint, [[maybe_unused]] int column,
                         const QString &text) {
    if (outpoint.isEmpty()) {
        return;
    }

    // Update the coin label through the controller
    if (m_controller == nullptr) {
        return;
    }
    m_controller->updateCoinLabel(outpoint, text);
}

} // namespace view
