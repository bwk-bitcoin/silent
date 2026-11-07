#include "account/views/Coins.h"

#include "account/RequestId.h"
#include "account/Same.h"
#include "account/views/TransactionDetails.h"
#include "catalog/BalanceHeader.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/feedback/Notice.h"
#include "catalog/panels/home/CoinsTable.h"
#include "i18n/Tr.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "views/utils.h"

#include <Qontrol>
#include <algorithm>
#include <common.h>

namespace account::view {

Coins::Coins(const Services &services) : m_services(services) {
    init();
    doConnect();
    view();
}

void Coins::init() {
    m_balance = m_services.blockchain->balance();
    m_coins = readCoins();
    m_dates = readDates();
}

void Coins::doConnect() {
    // The row/input must return from its event before its widgets can be replaced.
    connect(this, &Coins::interactionFinished, this, &Coins::onInteractionFinished,
            Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    auto *blockchain = m_services.blockchain;
    connect(blockchain, &backend::IBlockchain::coinsChanged, this, &Coins::onFundsChanged,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::balanceChanged, this, &Coins::onFundsChanged,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::historyChanged, this, &Coins::onFundsChanged,
            qontrol::UNIQUE);
    connect(m_services.labels, &backend::ILabels::saved, this, &Coins::onLabelSaved,
            qontrol::UNIQUE);
    connect(m_services.labels, &backend::IService::error, this, &Coins::onLabelError,
            qontrol::UNIQUE);
}

void Coins::view() {
    auto *balance = new catalog::BalanceHeader;
    balance->setConfirmed(m_balance.confirmed, m_balance.confirmedCount);
    balance->setUnconfirmed(m_balance.unconfirmed, m_balance.unconfirmedCount);

    m_notice = new catalog::Notice(catalog::Notice::Tone::Error);
    m_notice->setTitle(TR("coins-label-failed"));
    m_notice->setText(m_label_error);
    m_notice->setVisible(!m_label_error.isEmpty());

    m_table = new catalog::CoinsTable(m_compact);
    for (const auto &coin : m_coins) {
        m_table->addCoin(coin, m_dates.value(coin.outpoint.section(':', 0, 0)));
    }
    connect(m_table.data(), &catalog::CoinsTable::rowClicked, this, &Coins::onRowClicked,
            qontrol::UNIQUE);
    connect(m_table.data(), &catalog::CoinsTable::labelEdited, this, &Coins::onCoinLabelEdited,
            qontrol::UNIQUE);
    connect(m_table.data(), &catalog::CoinsTable::interactionFinished, this,
            &Coins::interactionFinished, qontrol::UNIQUE);

    auto *mainLayout =
        (new qontrol::Column(this))->push(balance)->push(m_notice.data())->push(m_table.data());
    setScreenContent(this, m_main_widget, dashboard(TR("coins-title"), mainLayout));
}

void Coins::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        refresh();
    }
    qontrol::Screen::changeEvent(event);
}

void Coins::resizeEvent(QResizeEvent *event) {
    qontrol::Screen::resizeEvent(event);
    bool compact = catalog::compactDates(this);
    if (compact != m_compact) {
        m_compact = compact;
        refresh();
    }
}

auto Coins::readCoins() const -> QList<backend::Coin> {
    QList<backend::Coin> coins;
    for (const auto &coin : m_services.blockchain->coins()) {
        if (!coin.spent) {
            coins.append(coin);
        }
    }
    std::ranges::stable_partition(
        coins, [](const backend::Coin &coin) { return !coin.height.has_value(); });
    return coins;
}

auto Coins::readDates() const -> QHash<QString, quint64> {
    QHash<QString, quint64> dates;
    for (const auto &tx : m_services.blockchain->history()) {
        if (tx.timestamp.has_value()) {
            dates.insert(tx.txid, *tx.timestamp);
        }
    }
    return dates;
}

void Coins::refresh() {
    if (isBusy()) {
        m_stale = true;
        return;
    }
    m_stale = false;
    view();
}

auto Coins::isBusy() const -> bool {
    return m_details_open || (m_table != nullptr && m_table->isEditing());
}

void Coins::showLabelResult(const QString &error) {
    // In place: an account that is not available answers from the edit's own call.
    m_label_error = error;
    if (m_notice != nullptr) {
        m_notice->setText(m_label_error);
        m_notice->setVisible(!m_label_error.isEmpty());
    }
}

// ===== Slots =====

void Coins::onInteractionFinished() {
    if (m_stale) {
        refresh();
    }
}

void Coins::onFundsChanged() {
    auto balance = m_services.blockchain->balance();
    auto coins = readCoins();
    auto dates = readDates();
    if (same(coins, m_coins) && same(balance, m_balance) && dates == m_dates && !m_stale) {
        return;
    }
    m_balance = balance;
    m_coins = coins;
    m_dates = dates;
    refresh();
}

void Coins::onLabelSaved(backend::RequestId request_id) {
    if (m_label_requests.remove(request_id)) {
        showLabelResult({});
    }
}

void Coins::onLabelError(backend::RequestId request_id, const QString &message) {
    if (m_label_requests.remove(request_id)) {
        showLabelResult(message);
    }
}

void Coins::onRowClicked(const QString &outpoint) {
    // The clicked row stays while the details are open: refresh() waits on it.
    m_details_open = true;
    QPointer<Coins> self(this);
    showTransactionDetails(m_services, outpoint.section(':', 0, 0));
    if (self != nullptr) {
        m_details_open = false;
        emit interactionFinished();
    }
}

void Coins::onCoinLabelEdited(const QString &outpoint, const QString &label) {
    auto requestId = nextRequestId();
    // Before the request: the answer may come during the call.
    m_label_requests.insert(requestId);
    m_services.labels->setLabel(requestId, {.kind = backend::LabelKind::Outpoint, .id = outpoint},
                                label);
}

} // namespace account::view
