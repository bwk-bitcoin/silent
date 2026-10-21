#include "account/views/History.h"

#include "account/Equality.h"
#include "account/RequestId.h"
#include "account/views/TransactionDetails.h"
#include "catalog/BalanceHeader.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/feedback/Notice.h"
#include "catalog/panels/home/HistoryTable.h"
#include "i18n/Tr.h"
#include "views/utils.h"

#include <Qontrol>
#include <algorithm>
#include <common.h>

namespace account::view {

History::History(AccountBackend *backend) : m_backend(backend) {
    init();
    doConnect();
    view();
}

void History::init() {
    m_history = readHistory();
}

void History::doConnect() {
    // The row/input must return from its event before its widgets can be replaced.
    connect(this, &History::interactionFinished, this, &History::onInteractionFinished,
            Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(m_backend, &AccountBackend::updateCoins, this, &History::onCoinsUpdated,
            qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::scanProgress, this, &History::onScanProgress,
            qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::endpointStateChanged, this,
            &History::onEndpointStateChanged, qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::endpointTipReady, this, &History::onEndpointTipReady,
            qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::labelsChanged, this, &History::onLabelsChanged,
            qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::labelEdited, this, &History::onLabelEdited,
            qontrol::UNIQUE);
}

void History::view() {
    auto *balance = new catalog::BalanceHeader;
    balance->setConfirmed(m_state.confirmed_balance, m_state.confirmed_count);
    balance->setUnconfirmed(m_state.unconfirmed_balance, m_state.unconfirmed_count);

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

auto History::readHistory() const -> QList<RustTx> {
    QList<RustTx> history;
    for (const auto &tx : m_backend->getPaymentHistory()) {
        history.append(tx);
    }
    std::ranges::stable_partition(history, [](const RustTx &tx) { return tx.height == 0; });
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
    for (auto endpoint : m_backend->endpoints()) {
        if (m_backend->isEndpointRunning(endpoint)) {
            m_tip_request = nextRequestId();
            m_backend->requestEndpointTip(m_tip_request, endpoint);
            return;
        }
    }
}

// ===== Slots =====

void History::onInteractionFinished() {
    if (m_stale) {
        refresh();
    }
}

void History::onCoinsUpdated(const CoinState &state) {
    requestTip();
    auto history = readHistory();
    bool txsChanged = history != m_history;
    if (!txsChanged && state == m_state && !m_stale) {
        return;
    }
    m_state = state;
    m_history = history;
    refresh();
}

void History::onScanProgress([[maybe_unused]] uint32_t height, uint32_t tip) {
    if (tip == 0 || tip == m_tip) {
        return;
    }
    m_tip = tip;
    refresh();
}

void History::onEndpointStateChanged(Endpoint endpoint) {
    if (m_backend->isEndpointRunning(endpoint)) {
        requestTip();
    }
}

void History::onEndpointTipReady(uint64_t req_id, [[maybe_unused]] Endpoint endpoint,
                                 const ElectrumTipResult &result) {
    if (req_id != m_tip_request) {
        return;
    }
    m_tip_request = 0;
    // Without a tip the confirmation dots stay hidden.
    if (!result.is_ok || result.height == m_tip) {
        return;
    }
    m_tip = result.height;
    refresh();
}

void History::onLabelsChanged() {
    m_history = readHistory();
    refresh();
}

void History::onLabelEdited(uint64_t req_id, bool accepted, const QString &error) {
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

void History::onRowClicked(const QString &txid) {
    // The clicked row stays while the details are open: refresh() waits on it.
    m_details_open = true;
    QPointer<History> self(this);
    showTransactionDetails(m_backend, txid);
    if (self != nullptr) {
        m_details_open = false;
        emit interactionFinished();
    }
}

void History::onTxLabelEdited(const QString &txid, const QString &label) {
    auto reqId = nextRequestId();
    m_label_requests.insert(reqId);
    m_backend->editTxLabel(reqId, txid, label);
}

} // namespace account::view
