#include "account/services/ServiceAdapters.h"

#include "account/RequestId.h"
#include "account/services/ServiceContext.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QPointer>
#include <QString>
#include <common.h>
#include <optional>
#include <utility>

namespace account {
namespace {

auto closed() -> QString {
    return QStringLiteral("The account is closed");
}

// What every adapter shares: the context, the requests waiting for it, and the
// extension requests, which name no operation of a built-in account.
template <typename Service> class Adapter : public Service {
public:
    Adapter(ServiceContext *context, QObject *parent) : Service(parent), m_context(context) {
    }

    void raw(backend::RequestId request_id, QString operation,
             [[maybe_unused]] QByteArray payload) override {
        emit this->error(request_id, unknownOperation(operation));
    }

    void value(backend::RequestId request_id, QString operation,
               [[maybe_unused]] QJsonValue payload) override {
        emit this->error(request_id, unknownOperation(operation));
    }

protected:
    [[nodiscard]] auto context() const -> ServiceContext * {
        return m_context.data();
    }

    // The context token of `request`, kept until its answer. Registered before the
    // request is sent: the answer may come during the call.
    auto track(backend::RequestId request) -> backend::RequestId {
        auto token = nextRequestId();
        m_pending.insert(token, request);
        return token;
    }

    // The request `token` answers, nullopt for another adapter's.
    auto take(backend::RequestId token) -> std::optional<backend::RequestId> {
        auto found = m_pending.find(token);
        if (found == m_pending.end()) {
            return std::nullopt;
        }
        auto request = found.value();
        m_pending.erase(found);
        return request;
    }

    void fail(backend::RequestId token, const QString &message) {
        if (auto request = take(token); request.has_value()) {
            emit this->error(*request, message);
        }
    }

    // The context is gone: nothing pending will be answered by it.
    void failPending() {
        auto pending = std::exchange(m_pending, {});
        for (auto request : pending) {
            emit this->error(request, closed());
        }
    }

    static auto unknownOperation(const QString &operation) -> QString {
        return QStringLiteral("This account provides no '%1' operation").arg(operation);
    }

private:
    QPointer<ServiceContext> m_context;
    QHash<backend::RequestId, backend::RequestId> m_pending;
};

class AccountService final : public Adapter<backend::IAccountBackend> {
    Q_OBJECT
    Q_INTERFACES(backend::IAccountBackend)

public:
    AccountService(ServiceContext *context, QObject *parent)
        : Adapter(context, parent),
          m_account(context->account()),
          m_network(context->network()) {
        connect(context, &ServiceContext::addressesChanged, this,
                &IAccountBackend::addressesChanged, qontrol::UNIQUE);
        connect(context, &ServiceContext::addressCreated, this, &AccountService::onAddressCreated,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::failed, this, &AccountService::onFailed, qontrol::UNIQUE);
        connect(context, &QObject::destroyed, this, &AccountService::onContextDestroyed,
                qontrol::UNIQUE);
    }

    [[nodiscard]] auto id() const -> QString override {
        return m_account;
    }

    // A built-in account is named by its id.
    [[nodiscard]] auto name() const -> QString override {
        return m_account;
    }

    [[nodiscard]] auto network() const -> backend::Network override {
        return m_network;
    }

    [[nodiscard]] auto addressKinds() const -> QList<backend::AddressKind> override {
        return context() == nullptr ? QList<backend::AddressKind>{} : context()->addressKinds();
    }

    [[nodiscard]] auto addresses() const -> QList<backend::Address> override {
        return context() == nullptr ? QList<backend::Address>{} : context()->addresses();
    }

    [[nodiscard]] auto addressVerification(const QString &address) const
        -> std::optional<backend::AddressVerification> override {
        if (context() == nullptr) {
            return std::nullopt;
        }
        return context()->addressVerification(address);
    }

    void newAddress(backend::RequestId request_id, QString kind) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->newAddress(track(request_id), kind);
    }

    // Account backups arrive with the backup and restore flows.
    void backup(backend::RequestId request_id, [[maybe_unused]] QByteArray password) override {
        emit error(request_id, backupUnsupported());
    }

    void inspectBackup(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                       [[maybe_unused]] QByteArray password) override {
        emit error(request_id, backupUnsupported());
    }

    void restore(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                 [[maybe_unused]] QByteArray password) override {
        emit error(request_id, backupUnsupported());
    }

public slots:

    void onAddressCreated(backend::RequestId token, const backend::Address &address) {
        if (auto request = take(token); request.has_value()) {
            emit addressCreated(*request, address);
        }
    }

    void onFailed(backend::RequestId token, const QString &message) {
        fail(token, message);
    }

    void onContextDestroyed() {
        failPending();
    }

protected:
    static auto backupUnsupported() -> QString {
        return QStringLiteral("Account backups are not supported yet");
    }

private:
    QString m_account;
    backend::Network m_network;
};

class BlockchainService final : public Adapter<backend::IBlockchain> {
    Q_OBJECT
    Q_INTERFACES(backend::IBlockchain)

public:
    BlockchainService(ServiceContext *context, QObject *parent) : Adapter(context, parent) {
        connect(context, &ServiceContext::stateChanged, this, &IBlockchain::stateChanged,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::coinsChanged, this, &IBlockchain::coinsChanged,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::balanceChanged, this, &IBlockchain::balanceChanged,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::historyChanged, this, &IBlockchain::historyChanged,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::transactionDetailsChanged, this,
                &IBlockchain::transactionDetailsChanged, qontrol::UNIQUE);
        connect(context, &ServiceContext::completed, this, &BlockchainService::onCompleted,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::transactionDetailsFetched, this,
                &BlockchainService::onTransactionDetailsFetched, qontrol::UNIQUE);
        connect(context, &ServiceContext::broadcastFinished, this,
                &BlockchainService::onBroadcastFinished, qontrol::UNIQUE);
        connect(context, &ServiceContext::failed, this, &BlockchainService::onFailed,
                qontrol::UNIQUE);
        connect(context, &QObject::destroyed, this, &BlockchainService::onContextDestroyed,
                qontrol::UNIQUE);
    }

    [[nodiscard]] auto state() const -> backend::BlockchainState override {
        return context() == nullptr ? backend::BlockchainState{} : context()->state();
    }

    [[nodiscard]] auto coins() const -> QList<backend::Coin> override {
        return context() == nullptr ? QList<backend::Coin>{} : context()->coins();
    }

    [[nodiscard]] auto balance() const -> backend::Balance override {
        return context() == nullptr ? backend::Balance{} : context()->balance();
    }

    [[nodiscard]] auto history() const -> QList<backend::Transaction> override {
        return context() == nullptr ? QList<backend::Transaction>{} : context()->history();
    }

    [[nodiscard]] auto transactionDetails(const QString &txid) const
        -> std::optional<backend::TransactionDetails> override {
        if (context() == nullptr) {
            return std::nullopt;
        }
        return context()->transactionDetails(txid);
    }

    void start(backend::RequestId request_id) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->connectAll(track(request_id), true);
    }

    void stop(backend::RequestId request_id) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->connectAll(track(request_id), false);
    }

    void rescan(backend::RequestId request_id,
                [[maybe_unused]] std::optional<quint64> from_height) override {
        emit error(request_id, QStringLiteral("Rescanning is not supported by this account"));
    }

    void setConnected(backend::RequestId request_id, QString connection_id,
                      bool connected) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->setConnected(track(request_id), connection_id, connected);
    }

    void refreshTip(backend::RequestId request_id, QString connection_id) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->refreshTip(track(request_id), connection_id);
    }

    void fetchTransactionDetails(backend::RequestId request_id, QString txid) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->fetchTransactionDetails(track(request_id), txid);
    }

    void broadcast(backend::RequestId request_id, QByteArray transaction) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->broadcast(track(request_id), transaction);
    }

public slots:

    void onCompleted(backend::RequestId token) {
        if (auto request = take(token); request.has_value()) {
            emit completed(*request);
        }
    }

    void onTransactionDetailsFetched(backend::RequestId token,
                                     const backend::TransactionDetails &details) {
        if (auto request = take(token); request.has_value()) {
            emit transactionDetailsFetched(*request, details);
        }
    }

    void onBroadcastFinished(backend::RequestId token, const backend::BroadcastResult &result) {
        if (auto request = take(token); request.has_value()) {
            emit broadcastFinished(*request, result);
        }
    }

    void onFailed(backend::RequestId token, const QString &message) {
        fail(token, message);
    }

    void onContextDestroyed() {
        failPending();
    }
};

class TransactionService final : public Adapter<backend::ITransaction> {
    Q_OBJECT
    Q_INTERFACES(backend::ITransaction)

public:
    TransactionService(ServiceContext *context, QObject *parent) : Adapter(context, parent) {
        connect(context, &ServiceContext::coinsSelected, this, &TransactionService::onCoinsSelected,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::failed, this, &TransactionService::onFailed,
                qontrol::UNIQUE);
        connect(context, &QObject::destroyed, this, &TransactionService::onContextDestroyed,
                qontrol::UNIQUE);
    }

    [[nodiscard]] auto craft(const backend::TransactionTemplate &transaction) const
        -> backend::CraftResult override {
        if (context() == nullptr) {
            return {.transaction = transaction, .completion = {.error = closed()}};
        }
        return context()->craft(transaction);
    }

    [[nodiscard]] auto finalize(const backend::TransactionTemplate &transaction) const
        -> std::optional<QByteArray> override {
        if (context() == nullptr) {
            return std::nullopt;
        }
        return context()->finalize(transaction);
    }

    void selectCoins(backend::RequestId request_id,
                     backend::TransactionTemplate transaction) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->selectCoins(track(request_id), transaction);
    }

public slots:

    void onCoinsSelected(backend::RequestId token, const backend::CraftResult &result) {
        if (auto request = take(token); request.has_value()) {
            emit coinsSelected(*request, result);
        }
    }

    void onFailed(backend::RequestId token, const QString &message) {
        fail(token, message);
    }

    void onContextDestroyed() {
        failPending();
    }
};

class LabelsService final : public Adapter<backend::ILabels> {
    Q_OBJECT
    Q_INTERFACES(backend::ILabels)

public:
    LabelsService(ServiceContext *context, QObject *parent) : Adapter(context, parent) {
        connect(context, &ServiceContext::labelsChanged, this, &ILabels::labelsChanged,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::labelSaved, this, &LabelsService::onLabelSaved,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::failed, this, &LabelsService::onFailed, qontrol::UNIQUE);
        connect(context, &QObject::destroyed, this, &LabelsService::onContextDestroyed,
                qontrol::UNIQUE);
    }

    [[nodiscard]] auto labels() const -> QList<backend::Label> override {
        return context() == nullptr ? QList<backend::Label>{} : context()->labels();
    }

    [[nodiscard]] auto label(const backend::LabelKey &key) const -> QString override {
        return context() == nullptr ? QString() : context()->label(key);
    }

    // An empty text removes the label. A save that fails after the RAM edit shows the
    // edit (labelsChanged()) and answers error(), not saved().
    void setLabel(backend::RequestId request_id, backend::LabelKey key, QString text) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->setLabel(track(request_id), key, text);
    }

public slots:

    void onLabelSaved(backend::RequestId token) {
        if (auto request = take(token); request.has_value()) {
            emit saved(*request);
        }
    }

    void onFailed(backend::RequestId token, const QString &message) {
        fail(token, message);
    }

    void onContextDestroyed() {
        failPending();
    }
};

class SettingsService final : public Adapter<backend::IBackendSettings> {
    Q_OBJECT
    Q_INTERFACES(backend::IBackendSettings)

public:
    SettingsService(ServiceContext *context, QObject *parent) : Adapter(context, parent) {
        connect(context, &ServiceContext::settingsChanged, this, &IBackendSettings::settingsChanged,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::endpointTested, this, &SettingsService::onEndpointTested,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::completed, this, &SettingsService::onCompleted,
                qontrol::UNIQUE);
        connect(context, &ServiceContext::failed, this, &SettingsService::onFailed,
                qontrol::UNIQUE);
        connect(context, &QObject::destroyed, this, &SettingsService::onContextDestroyed,
                qontrol::UNIQUE);
    }

    [[nodiscard]] auto settings() const -> backend::Settings override {
        return context() == nullptr ? backend::Settings{} : context()->settings();
    }

    // Tests against the account's network, saving nothing and leaving the live
    // connection alone.
    void testEndpoint(backend::RequestId request_id, backend::Endpoint endpoint) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->testEndpoint(track(request_id), endpoint);
    }

    // Persists the endpoints untested; IBlockchain reconnects to them.
    void save(backend::RequestId request_id, backend::Settings settings) override {
        if (context() == nullptr) {
            emit error(request_id, closed());
            return;
        }
        context()->saveSettings(track(request_id), settings);
    }

public slots:

    void onEndpointTested(backend::RequestId token) {
        if (auto request = take(token); request.has_value()) {
            emit endpointTested(*request);
        }
    }

    void onCompleted(backend::RequestId token) {
        if (auto request = take(token); request.has_value()) {
            emit saved(*request);
        }
    }

    void onFailed(backend::RequestId token, const QString &message) {
        fail(token, message);
    }

    void onContextDestroyed() {
        failPending();
    }
};

} // namespace

auto serviceAdapter(backend::ServiceKind kind, ServiceContext *context, QObject *parent)
    -> backend::IService * {
    switch (kind) {
    case backend::ServiceKind::Account:
        return new AccountService(context, parent);
    case backend::ServiceKind::Blockchain:
        return new BlockchainService(context, parent);
    case backend::ServiceKind::Transaction:
        return new TransactionService(context, parent);
    case backend::ServiceKind::Labels:
        return new LabelsService(context, parent);
    case backend::ServiceKind::Settings:
        return new SettingsService(context, parent);
    }
    return nullptr;
}

} // namespace account

#include "ServiceAdapters.moc"
