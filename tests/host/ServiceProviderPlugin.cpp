// An account service provider built against the published SDK only. Built twice:
// services.full supplies every service, services.chain (SILENT_SERVICE_CHAIN_FIXTURE)
// only the blockchain and labels, and deletes a service it is handed back only a while
// later. Every getter answers this plugin's id so a consumer
// can tell which provider served it. Payloads are heap copies: they outlive no unload.

#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/provider.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "interfaces/module.h"

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <optional>
#include <utility>

namespace {

#ifdef SILENT_SERVICE_CHAIN_FIXTURE
constexpr auto PLUGIN_ID = "services.chain";
// How long a released service lives on.
constexpr int HELD_MS = 1500;
#else
constexpr auto PLUGIN_ID = "services.full";
#endif

auto pluginId() -> QString {
    return QString::fromLatin1(PLUGIN_ID);
}

auto refusal() -> QString {
    return pluginId() + QString::fromLatin1(" refuses this request");
}

auto supplies(backend::ServiceKind kind) -> bool {
#ifdef SILENT_SERVICE_CHAIN_FIXTURE
    return kind == backend::ServiceKind::Blockchain || kind == backend::ServiceKind::Labels;
#else
    Q_UNUSED(kind);
    return true;
#endif
}

// Both extensions echo their payload, so a consumer sees the request reach this plugin.
template <typename Service> class FixtureService : public Service {
public:
    explicit FixtureService(QObject *parent) : Service(parent) {
    }

    void raw(backend::RequestId request_id, [[maybe_unused]] QString operation,
             QByteArray payload) override {
        emit this->rawResult(request_id, payload);
    }

    void value(backend::RequestId request_id, [[maybe_unused]] QString operation,
               QJsonValue payload) override {
        emit this->valueResult(request_id, payload);
    }
};

} // namespace

class Q_DECL_HIDDEN FixtureAccount final : public FixtureService<backend::IAccountBackend> {
    Q_OBJECT
    Q_INTERFACES(backend::IAccountBackend)

public:
    FixtureAccount(QString account, QObject *parent)
        : FixtureService(parent),
          m_account(std::move(account)) {
    }

    [[nodiscard]] auto id() const -> QString override {
        return m_account;
    }

    [[nodiscard]] auto name() const -> QString override {
        return pluginId();
    }

    // Fixed, so a request for another network is refused at assembly.
    [[nodiscard]] auto network() const -> backend::Network override {
        return backend::Network::Regtest;
    }

    [[nodiscard]] auto addressKinds() const -> QList<backend::AddressKind> override {
        return {};
    }

    [[nodiscard]] auto addresses() const -> QList<backend::Address> override {
        return {};
    }

    [[nodiscard]] auto addressVerification([[maybe_unused]] const QString &address) const
        -> std::optional<backend::AddressVerification> override {
        return std::nullopt;
    }

    void newAddress(backend::RequestId request_id, [[maybe_unused]] QString kind) override {
        emit error(request_id, refusal());
    }

    void backup(backend::RequestId request_id, [[maybe_unused]] QByteArray password) override {
        emit error(request_id, refusal());
    }

    void inspectBackup(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                       [[maybe_unused]] QByteArray password) override {
        emit error(request_id, refusal());
    }

    void restore(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                 [[maybe_unused]] QByteArray password) override {
        emit error(request_id, refusal());
    }

private:
    QString m_account;
};

class Q_DECL_HIDDEN FixtureBlockchain final : public FixtureService<backend::IBlockchain> {
    Q_OBJECT
    Q_INTERFACES(backend::IBlockchain)

public:
    using FixtureService::FixtureService;

    [[nodiscard]] auto state() const -> backend::BlockchainState override {
        return {};
    }

    [[nodiscard]] auto coins() const -> QList<backend::Coin> override {
        return {};
    }

    [[nodiscard]] auto balance() const -> backend::Balance override {
        return {};
    }

    [[nodiscard]] auto history() const -> QList<backend::Transaction> override {
        return {backend::Transaction{.txid = pluginId()}};
    }

    [[nodiscard]] auto transactionDetails([[maybe_unused]] const QString &txid) const
        -> std::optional<backend::TransactionDetails> override {
        return std::nullopt;
    }

    void start(backend::RequestId request_id) override {
        emit completed(request_id);
    }

    void stop(backend::RequestId request_id) override {
        emit completed(request_id);
    }

    void rescan(backend::RequestId request_id,
                [[maybe_unused]] std::optional<quint64> from_height) override {
        emit error(request_id, refusal());
    }

    void setConnected(backend::RequestId request_id, [[maybe_unused]] QString connection_id,
                      [[maybe_unused]] bool connected) override {
        emit error(request_id, refusal());
    }

    void refreshTip(backend::RequestId request_id,
                    [[maybe_unused]] QString connection_id) override {
        emit error(request_id, refusal());
    }

    void fetchTransactionDetails(backend::RequestId request_id,
                                 [[maybe_unused]] QString txid) override {
        emit error(request_id, refusal());
    }

    void broadcast(backend::RequestId request_id,
                   [[maybe_unused]] QByteArray transaction) override {
        emit error(request_id, refusal());
    }
};

class Q_DECL_HIDDEN FixtureTransaction final : public FixtureService<backend::ITransaction> {
    Q_OBJECT
    Q_INTERFACES(backend::ITransaction)

public:
    using FixtureService::FixtureService;

    [[nodiscard]] auto craft(const backend::TransactionTemplate &transaction) const
        -> backend::CraftResult override {
        return {.transaction = transaction, .completion = {.complete = true, .error = {}}};
    }

    [[nodiscard]] auto
    finalize([[maybe_unused]] const backend::TransactionTemplate &transaction) const
        -> std::optional<QByteArray> override {
        return std::nullopt;
    }

    void selectCoins(backend::RequestId request_id,
                     backend::TransactionTemplate transaction) override {
        emit coinsSelected(request_id, craft(transaction));
    }
};

class Q_DECL_HIDDEN FixtureLabels final : public FixtureService<backend::ILabels> {
    Q_OBJECT
    Q_INTERFACES(backend::ILabels)

public:
    using FixtureService::FixtureService;

    [[nodiscard]] auto labels() const -> QList<backend::Label> override {
        return {};
    }

    [[nodiscard]] auto label([[maybe_unused]] const backend::LabelKey &key) const
        -> QString override {
        return pluginId();
    }

    void setLabel(backend::RequestId request_id, [[maybe_unused]] backend::LabelKey key,
                  [[maybe_unused]] QString text) override {
        emit saved(request_id);
    }
};

class Q_DECL_HIDDEN FixtureSettings final : public FixtureService<backend::IBackendSettings> {
    Q_OBJECT
    Q_INTERFACES(backend::IBackendSettings)

public:
    using FixtureService::FixtureService;

    [[nodiscard]] auto settings() const -> backend::Settings override {
        return {.network = backend::Network::Regtest,
                .endpoints = {backend::Endpoint{.id = pluginId()}}};
    }

    void testEndpoint(backend::RequestId request_id,
                      [[maybe_unused]] backend::Endpoint endpoint) override {
        emit endpointTested(request_id);
    }

    void save(backend::RequestId request_id, [[maybe_unused]] backend::Settings settings) override {
        emit error(request_id, refusal());
    }
};

class Q_DECL_HIDDEN FixtureModule final : public IModule, public backend::IServiceProvider {
    Q_OBJECT
    Q_INTERFACES(backend::IServiceProvider)

public:
    using IModule::IModule;

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QString::fromLatin1("id"), pluginId() + QString::fromLatin1(".module")}};
    }

    auto list() -> ReqId override {
        return 0;
    }

    auto autoStart() -> ReqId override {
        return 0;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }

    // The host hands every service back before it asks: one still out is a host bug.
    void shutdown(ReqId req_id) override {
        emit shutdownFinished(req_id, m_outstanding == 0
                                          ? QString()
                                          : QString::fromLatin1("services still handed out"));
    }

    [[nodiscard]] auto service(const QString &account, [[maybe_unused]] backend::Network network,
                               backend::ServiceKind kind) -> backend::IService * override {
        if (!supplies(kind)) {
            return nullptr;
        }
        backend::IService *service = nullptr;
        switch (kind) {
        case backend::ServiceKind::Account:
            service = new FixtureAccount(account, this);
            break;
        case backend::ServiceKind::Blockchain:
            service = new FixtureBlockchain(this);
            break;
        case backend::ServiceKind::Transaction:
            service = new FixtureTransaction(this);
            break;
        case backend::ServiceKind::Labels:
            service = new FixtureLabels(this);
            break;
        case backend::ServiceKind::Settings:
            service = new FixtureSettings(this);
            break;
        }
        setProperty("outstanding", ++m_outstanding);
        return service;
    }

    void releaseService(backend::IService *service) override {
        setProperty("outstanding", --m_outstanding);
#ifdef SILENT_SERVICE_CHAIN_FIXTURE
        // Deleted a while after its release: the plugin must stay loaded until it is.
        QTimer::singleShot(HELD_MS, service, &QObject::deleteLater);
#else
        service->deleteLater();
#endif
    }

private:
    int m_outstanding = 0;
};

class Q_DECL_HIDDEN FixturePlugin final : public QObject, public IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SILENT_IPlugin_IID)
    Q_INTERFACES(IPlugin)

public:
    FixturePlugin() : m_module(new FixtureModule(this)) {
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QString::fromLatin1("id"), pluginId()}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {m_module};
    }

private:
    FixtureModule *m_module;
};

#include "ServiceProviderPlugin.moc"
