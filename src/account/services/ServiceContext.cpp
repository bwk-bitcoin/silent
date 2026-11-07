#include "account/services/ServiceContext.h"

#include "account/services/Conversions.h"

#include <QMutexLocker>
#include <QTimer>
#include <common.h>
#include <utility>

namespace account {
namespace {

// How often, and how many times, funds whose history lags their coins are read again.
constexpr int RECONCILE_INTERVAL_MS = 1000;
constexpr int RECONCILE_ATTEMPTS = 120;

} // namespace

ServiceContext::ServiceContext(QString account, QObject *parent)
    : QObject(parent),
      m_account(std::move(account)),
      m_reconcile(new QTimer(this)) {
    m_reconcile->setSingleShot(true);
    m_reconcile->setInterval(RECONCILE_INTERVAL_MS);
    connect(m_reconcile, &QTimer::timeout, this, &ServiceContext::onReconcileTimeout,
            qontrol::UNIQUE);
}

auto ServiceContext::account() const -> QString {
    return m_account;
}

auto ServiceContext::network() const -> backend::Network {
    QMutexLocker lock(&m_mutex);
    return m_network;
}

auto ServiceContext::coins() const -> QList<backend::Coin> {
    QMutexLocker lock(&m_mutex);
    return m_funds.coins;
}

auto ServiceContext::balance() const -> backend::Balance {
    QMutexLocker lock(&m_mutex);
    return m_funds.balance;
}

auto ServiceContext::history() const -> QList<backend::Transaction> {
    QMutexLocker lock(&m_mutex);
    return m_funds.history;
}

auto ServiceContext::transactionDetails(const QString &txid) const
    -> std::optional<backend::TransactionDetails> {
    QMutexLocker lock(&m_mutex);
    auto found = m_details.constFind(txid);
    if (found == m_details.cend()) {
        return std::nullopt;
    }
    return found.value();
}

auto ServiceContext::labels() const -> QList<backend::Label> {
    QMutexLocker lock(&m_mutex);
    return m_labels;
}

auto ServiceContext::label(const backend::LabelKey &key) const -> QString {
    QMutexLocker lock(&m_mutex);
    for (const auto &label : m_labels) {
        if (label.key.kind == key.kind && label.key.id == key.id) {
            return label.text;
        }
    }
    return {};
}

auto ServiceContext::addressKinds() const -> QList<backend::AddressKind> {
    QMutexLocker lock(&m_mutex);
    return m_receiving.kinds;
}

auto ServiceContext::addresses() const -> QList<backend::Address> {
    QMutexLocker lock(&m_mutex);
    return m_receiving.addresses;
}

auto ServiceContext::addressVerification(const QString &address) const
    -> std::optional<backend::AddressVerification> {
    QMutexLocker lock(&m_mutex);
    auto found = m_receiving.verification.constFind(address);
    if (found == m_receiving.verification.cend()) {
        return std::nullopt;
    }
    return found.value();
}

auto ServiceContext::state() const -> backend::BlockchainState {
    QMutexLocker lock(&m_mutex);
    return m_state;
}

auto ServiceContext::settings() const -> backend::Settings {
    QMutexLocker lock(&m_mutex);
    return m_settings;
}

void ServiceContext::publishFunds(const Funds &funds) {
    bool coins = false;
    bool balance = false;
    bool history = false;
    {
        QMutexLocker lock(&m_mutex);
        coins = !services::same(m_funds.coins, funds.coins);
        balance = !services::same(m_funds.balance, funds.balance);
        history = !services::same(m_funds.history, funds.history);
        m_funds = funds;
    }
    m_history_pending = !funds.historyComplete;
    // Published before the notifications, so a notified consumer reads the new state.
    if (coins) {
        emit coinsChanged();
    }
    if (balance) {
        emit balanceChanged();
    }
    if (history) {
        emit historyChanged();
    }
}

void ServiceContext::publishNetwork(backend::Network network) {
    QMutexLocker lock(&m_mutex);
    m_network = network;
}

void ServiceContext::publishDetails(const backend::TransactionDetails &details) {
    {
        QMutexLocker lock(&m_mutex);
        m_details.insert(details.transaction.txid, details);
    }
    emit transactionDetailsChanged(details.transaction.txid);
}

void ServiceContext::publishState(const backend::BlockchainState &state) {
    {
        QMutexLocker lock(&m_mutex);
        if (services::same(m_state, state)) {
            return;
        }
        m_state = state;
    }
    emit stateChanged();
}

void ServiceContext::publishSettings(const backend::Settings &settings) {
    {
        QMutexLocker lock(&m_mutex);
        if (services::same(m_settings, settings)) {
            return;
        }
        m_settings = settings;
    }
    emit settingsChanged();
}

// An early coin is not a commit point: its history is read again until it follows.
void ServiceContext::reconcileFunds() {
    auto funds = readFunds();
    if (funds.has_value()) {
        publishFunds(*funds);
    }
    if (m_history_pending && m_reconcile_attempts < RECONCILE_ATTEMPTS) {
        m_reconcile->start();
    } else {
        m_reconcile->stop();
    }
}

// ===== Slots =====

void ServiceContext::onFundsChanged() {
    m_reconcile_attempts = 0;
    reconcileFunds();
}

void ServiceContext::onLabelsChanged() {
    auto labels = readLabels();
    if (!labels.has_value()) {
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        if (services::same(m_labels, *labels)) {
            return;
        }
        m_labels = *labels;
    }
    emit labelsChanged();
}

void ServiceContext::onReceivingChanged() {
    auto receiving = readReceiving();
    if (!receiving.has_value()) {
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        if (services::same(m_receiving.kinds, receiving->kinds) &&
            services::same(m_receiving.addresses, receiving->addresses)) {
            return;
        }
        m_receiving = *receiving;
    }
    emit addressesChanged();
}

void ServiceContext::onReconcileTimeout() {
    ++m_reconcile_attempts;
    reconcileFunds();
}

} // namespace account
