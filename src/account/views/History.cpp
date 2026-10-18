#include "account/views/History.h"

#include "account/AccountBackend.h"
#include "catalog/BalanceHeader.h"
#include "catalog/panels/home/HistoryTable.h"
#include "i18n/Tr.h"
#include "views/utils.h"

#include <Qontrol>
#include <common.h>

namespace account::view {

History::History(AccountBackend *backend) : m_backend(backend) {
    init();
    doConnect();
    view();
}

void History::init() {
}

void History::doConnect() {
    connect(m_backend, &AccountBackend::updateCoins, this, &History::onCoinsUpdated,
            qontrol::UNIQUE);
}

void History::view() {
    m_balance = new catalog::BalanceHeader;
    m_balance->setConfirmed(toBitcoin(m_state.confirmed_balance, false));
    m_balance->setUnconfirmed(m_state.unconfirmed_balance);
    m_balance->setCoinCount(m_state.confirmed_count + m_state.unconfirmed_count);

    m_table = new catalog::HistoryTable;
    for (const auto &tx : m_backend->getPaymentHistory()) {
        QString direction = QString::fromUtf8(tx.direction.data(), tx.direction.size());
        QString txid = QString::fromUtf8(tx.txid.data(), tx.txid.size());
        m_table->addEntry(direction, txid, tx.height, tx.amount);
    }

    auto *mainLayout = (new qontrol::Column(this))->push(m_balance)->push(m_table);
    setScreenContent(this, m_main_widget, dashboard(TR("history-title"), mainLayout));
}

void History::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        view();
    }
    qontrol::Screen::changeEvent(event);
}

// ===== Slots =====

void History::onCoinsUpdated(const CoinState &state) {
    m_state = state;
    view();
}

} // namespace account::view
