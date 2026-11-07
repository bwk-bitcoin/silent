#include "account/views/History.h"

#include "account/RequestId.h"
#include "account/Same.h"
#include "account/views/TransactionDetails.h"
#include "catalog/BalanceHeader.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/feedback/Notice.h"
#include "catalog/panels/home/HistoryTable.h"
#include "i18n/Tr.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "views/utils.h"

#include <Qontrol>
#include <algorithm>
#include <common.h>

namespace account::view {

History::History(const Services &services) : m_services(services) {
    init();
    doConnect();
    view();
}

void History::init() {
    m_balance = m_services.blockchain->balance();
    m_history = readHistory();
    auto state = m_services.blockchain->state();
    m_tip = stateTip(state);
    m_connected = connectedIds(state);
}

void History::doConnect() {
    // The row/input must return from its event before its widgets can be replaced.
    connect(this, &History::interactionFinished, this, &History::onInteractionFinished,
            Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    auto *blockchain = m_services.blockchain;
    connect(blockchain, &backend::IBlockchain::historyChanged, this, &History::onFundsChanged,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::balanceChanged, this, &History::onFundsChanged,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::stateChanged, this, &History::onStateChanged,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::completed, this, &History::onTipRefreshed,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IService::error, this, &History::onBlockchainError,
            qontrol::UNIQUE);
    connect(m_services.labels, &backend::ILabels::saved, this, &History::onLabelSaved,
            qontrol::UNIQUE);
    connect(m_services.labels, &backend::IService::error, this, &History::onLabelError,
            qontrol::UNIQUE);
}

void History::view() {
    auto *balance = new catalog::BalanceHeader;
    balance->setConfirmed(m_balance.confirmed, m_balance.confirmedCount);
    balance->setUnconfirmed(m_balance.unconfirmed, m_balance.unconfirmedCount);

    m_notice = new catalog::Notice(catalog::Notice::Tone::Error);
    m_notice->setTitle(TR("history-label-failed"));
    m_notice->setText(m_label_error);
    m_notice->setVisible(!m_label_error.isEmpty());

    m_table = new catalog::HistoryTable(m_compact);
    for (const auto &tx : m_history) {
        m_table->addEntry(tx, m_tip);
    }
    connect(m_table.data(), &catalog::HistoryTable::rowClicked, this, &History::onRowClicked,
            qontrol::UNIQUE);
    connect(m_table.data(), &catalog::HistoryTable::labelEdited, this, &History::onTxLabelEdited,
            qontrol::UNIQUE);
    connect(m_table.data(), &catalog::HistoryTable::interactionFinished, this,
            &History::interactionFinished, qontrol::UNIQUE);

    auto *mainLayout =
        (new qontrol::Column(this))->push(balance)->push(m_notice.data())->push(m_table.data());
    setScreenContent(this, m_main_widget, dashboard(TR("history-title"), mainLayout));
}

void History::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        refresh();
    }
    qontrol::Screen::changeEvent(event);
}

void History::resizeEvent(QResizeEvent *event) {
    qontrol::Screen::resizeEvent(event);
    bool compact = catalog::compactDates(this);
    if (compact != m_compact) {
        m_compact = compact;
        refresh();
    }
}

void History::showEvent(QShowEvent *event) {
    qontrol::Screen::showEvent(event);
    requestTip();
}

auto History::readHistory() const -> QList<backend::Transaction> {
    auto history = m_services.blockchain->history();
    std::ranges::stable_partition(
        history, [](const backend::Transaction &tx) { return !tx.height.has_value(); });
    return history;
}

void History::refresh() {
    if (isBusy()) {
        m_stale = true;
        return;
    }
    m_stale = false;
    view();
}

auto History::isBusy() const -> bool {
    return m_details_open || (m_table != nullptr && m_table->isEditing());
}

void History::requestTip() {
    if (!isVisible() || m_tip_request != 0) {
        return;
    }
    for (const auto &connection : m_services.blockchain->state().connections) {
        if (connection.connected) {
            m_tip_request = nextRequestId();
            m_services.blockchain->refreshTip(m_tip_request, connection.id);
            return;
        }
    }
}

// A new tip counts the confirmations again. Without a tip the confirmation dots stay
// hidden.
void History::updateTip(uint32_t tip) {
    if (tip == 0 || tip == m_tip) {
        return;
    }
    m_tip = tip;
    refresh();
}

auto History::connectedIds(const backend::BlockchainState &state) -> QSet<QString> {
    QSet<QString> ids;
    for (const auto &connection : state.connections) {
        if (connection.connected) {
            ids.insert(connection.id);
        }
    }
    return ids;
}

void History::showLabelResult(const QString &error) {
    // In place: an account that is not available answers from the edit's own call.
    m_label_error = error;
    if (m_notice != nullptr) {
        m_notice->setText(m_label_error);
        m_notice->setVisible(!m_label_error.isEmpty());
    }
}

// ===== Slots =====

void History::onInteractionFinished() {
    if (m_stale) {
        refresh();
    }
}

void History::onFundsChanged() {
    requestTip();
    auto balance = m_services.blockchain->balance();
    auto history = readHistory();
    if (same(history, m_history) && same(balance, m_balance) && !m_stale) {
        return;
    }
    m_balance = balance;
    m_history = history;
    refresh();
}

// A connection that comes up is asked its tip.
void History::onStateChanged() {
    auto state = m_services.blockchain->state();
    auto connected = connectedIds(state);
    bool connecting = !(connected - m_connected).isEmpty();
    m_connected = connected;
    if (connecting) {
        requestTip();
    }
    updateTip(stateTip(state));
}

void History::onTipRefreshed(backend::RequestId request_id) {
    if (request_id == m_tip_request) {
        m_tip_request = 0;
        updateTip(stateTip(m_services.blockchain->state()));
    }
}

void History::onBlockchainError(backend::RequestId request_id) {
    if (request_id == m_tip_request) {
        m_tip_request = 0;
    }
}

void History::onLabelSaved(backend::RequestId request_id) {
    if (m_label_requests.remove(request_id)) {
        showLabelResult({});
    }
}

void History::onLabelError(backend::RequestId request_id, const QString &message) {
    if (m_label_requests.remove(request_id)) {
        showLabelResult(message);
    }
}

void History::onRowClicked(const QString &txid) {
    // The clicked row stays while the details are open: refresh() waits on it.
    m_details_open = true;
    QPointer<History> self(this);
    showTransactionDetails(m_services, txid);
    if (self != nullptr) {
        m_details_open = false;
        emit interactionFinished();
    }
}

void History::onTxLabelEdited(const QString &txid, const QString &label) {
    auto requestId = nextRequestId();
    // Before the request: the answer may come during the call.
    m_label_requests.insert(requestId);
    m_services.labels->setLabel(requestId, {.kind = backend::LabelKind::Transaction, .id = txid},
                                label);
}

} // namespace account::view
