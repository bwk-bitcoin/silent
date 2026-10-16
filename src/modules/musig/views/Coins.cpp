#include "modules/musig/views/Coins.h"

#include "catalog/BalanceHeader.h"
#include "catalog/Table.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "views/utils.h"

#include <Qontrol>
#include <common.h>

namespace musig::view {

using catalog::DisplayRole;

Coins::Coins(MusigAccountController *ctrl) : m_controller(ctrl) {
    init();
    doConnect();
    view();
}

void Coins::init() {
}

void Coins::doConnect() {
    connect(m_controller, &MusigAccountController::updateCoins, this, &Coins::onCoinsUpdated,
            qontrol::UNIQUE);
}

void Coins::view() {
    if (m_controller == nullptr) {
        return;
    }

    m_balance = new catalog::BalanceHeader;
    m_balance->setConfirmed(toBitcoin(m_state.confirmed_balance, false));
    m_balance->setUnconfirmed(m_state.unconfirmed_balance);
    m_balance->setCoinCount(m_state.confirmed_count + m_state.unconfirmed_count);

    m_table = new catalog::Table;
    m_table->addColumn({.title = TR("coins-type"), .width = TYPE_W});
    m_table->addColumn({.title = TR("coins-height"), .width = HEIGHT_W});
    m_table->addColumn(
        {.title = TR("coins-outpoint"), .width = OUTPOINT_W, .role = DisplayRole::Outpoint});
    m_table->addColumn({.title = TR("coins-label"),
                        .width = LABEL_W,
                        .grow = true,
                        .editable = true,
                        .placeholder = QStringLiteral("-")});
    m_table->addColumn({.title = TR("coins-value"),
                        .width = VALUE_W,
                        .alignment = Qt::AlignRight,
                        .headerAlignment = Qt::AlignRight,
                        .role = DisplayRole::Amount});
    connect(m_table, &catalog::Table::cellEdited, this, &Coins::onCellEdited, qontrol::UNIQUE);

    for (const auto &coin : m_controller->getCoins()) {
        QString typeStr = QString::fromUtf8(coin.account_type.data(), coin.account_type.size());
        QString heightStr =
            coin.height > 0 ? QString::number(coin.height) : TR("coins-unconfirmed-no-colon");
        QString outpoint = QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size());
        QString label = QString::fromUtf8(coin.label.data(), coin.label.size());
        m_table->addRow(outpoint, {typeStr, heightStr, shortenOutpoint(outpoint), label,
                                   toBitcoin(coin.value)});
    }

    auto *mainLayout = (new qontrol::Column(this))->push(m_balance)->push(m_table);
    setScreenContent(this, m_main_widget, dashboard(TR("coins-title"), mainLayout));
}

// ===== Slots =====

void Coins::onCoinsUpdated(CoinState state) {
    m_state = state;
    view();
}

void Coins::onCellEdited(const QString &outpoint, [[maybe_unused]] int column,
                         const QString &text) {
    onLabelEdited(outpoint, text);
}

void Coins::onLabelEdited(const QString &outpoint, const QString &label) {
    if (outpoint.isEmpty() || m_controller == nullptr) {
        return;
    }
    m_controller->updateCoinLabel(outpoint, label);
}

} // namespace musig::view
