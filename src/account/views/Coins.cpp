#include "account/views/Coins.h"

#include "account/AccountBackend.h"
#include "account/Equality.h"
#include "account/RequestId.h"
#include "account/views/TransactionDetails.h"
#include "catalog/BalanceHeader.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/feedback/Notice.h"
#include "catalog/panels/home/CoinsTable.h"
#include "i18n/Tr.h"
#include "views/utils.h"

#include <Qontrol>
#include <algorithm>
#include <common.h>

namespace account::view {

Coins::Coins(AccountBackend *backend) : m_backend(backend) {
    init();
    doConnect();
    view();
}

void Coins::init() {
    m_coins = readCoins();
}

void Coins::doConnect() {
    // The row/input must return from its event before its widgets can be replaced.
    connect(this, &Coins::interactionFinished, this, &Coins::onInteractionFinished,
            Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(m_backend, &AccountBackend::updateCoins, this, &Coins::onCoinsUpdated, qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::labelsChanged, this, &Coins::onLabelsChanged,
            qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::labelEdited, this, &Coins::onLabelEdited, qontrol::UNIQUE);
}

void Coins::view() {
    auto *balance = new catalog::BalanceHeader;
    balance->setConfirmed(m_state.confirmed_balance, m_state.confirmed_count);
    balance->setUnconfirmed(m_state.unconfirmed_balance, m_state.unconfirmed_count);

    m_notice = new catalog::Notice(catalog::Notice::Tone::Error);
    m_notice->setTitle(TR("coins-label-failed"));
    m_notice->setText(m_label_error);
    m_notice->setVisible(!m_label_error.isEmpty());

    m_table = new catalog::CoinsTable(m_compact);
    for (const auto &coin : m_coins) {
        m_table->addCoin(coin);
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

auto Coins::readCoins() const -> QList<RustCoin> {
    QList<RustCoin> coins;
    for (const auto &coin : m_backend->getCoins()) {
        coins.append(coin);
    }
    std::ranges::stable_partition(coins, [](const RustCoin &coin) { return coin.height == 0; });
    return coins;
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

// ===== Slots =====

void Coins::onInteractionFinished() {
    if (m_stale) {
        refresh();
    }
}

void Coins::onCoinsUpdated(const CoinState &state) {
    auto coins = readCoins();
    if (coins == m_coins && state == m_state && !m_stale) {
        return;
    }
    m_state = state;
    m_coins = coins;
    refresh();
}

void Coins::onLabelsChanged() {
    m_coins = readCoins();
    refresh();
}

void Coins::onLabelEdited(uint64_t req_id, bool accepted, const QString &error) {
    if (!m_label_requests.contains(req_id)) {
        return;
    }
    m_label_requests.remove(req_id);
    // In place: an account that is not available answers from the edit's own call.
    m_label_error = accepted ? QString() : error;
    if (m_notice != nullptr) {
        m_notice->setText(m_label_error);
        m_notice->setVisible(!accepted);
    }
}

void Coins::onRowClicked(const QString &outpoint) {
    // The clicked row stays while the details are open: refresh() waits on it.
    m_details_open = true;
    QPointer<Coins> self(this);
    showTransactionDetails(m_backend, outpoint.section(':', 0, 0));
    if (self != nullptr) {
        m_details_open = false;
        emit interactionFinished();
    }
}

void Coins::onCoinLabelEdited(const QString &outpoint, const QString &label) {
    auto reqId = nextRequestId();
    m_label_requests.insert(reqId);
    m_backend->editOutpointLabel(reqId, outpoint, label);
}

} // namespace account::view
