// An account service provider built against the published SDK only. Built twice:
// services.full supplies every service, services.chain (SILENT_SERVICE_CHAIN_FIXTURE)
// only the blockchain and labels, and deletes a service it is handed back only a while
// later. Every getter answers this plugin's id so a consumer
// can tell which provider served it. Payloads are heap copies: they outlive no unload.
// The module records the last AccountWatch it was given in its properties, and its
// blockchain reports one output per watched descriptor, paid at its birthday, unless a test
// sets what it reports in its fixtureOutputs, fixtureUsed and fixtureTip properties. The
// chain one alone advertises that its blockchain watches descriptors.

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
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
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

// What each output of the blockchain pays, in satoshis.
constexpr quint64 OUTPUT_SATS = 50000;

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

// Both extensions echo their payload as the result, then as an event of the operation, so a
// consumer sees the request reach this plugin.
template <typename Service> class FixtureService : public Service {
public:
    explicit FixtureService(QObject *parent) : Service(parent) {
    }

    void raw(backend::RequestId request_id, QString operation, QByteArray payload) override {
        emit this->rawResult(request_id, payload);
        emit this->rawEvent(operation, payload);
    }

    void value(backend::RequestId request_id, QString operation, QJsonValue payload) override {
        emit this->valueResult(request_id, payload);
        emit this->valueEvent(operation, payload);
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

    [[nodiscard]] auto backupDescription() const -> backend::BackupDescription override {
        return {};
    }

    void newAddress(backend::RequestId request_id, [[maybe_unused]] QString kind) override {
        emit error(request_id, refusal());
    }

    void backup(backend::RequestId request_id) override {
        emit error(request_id, refusal());
    }

    void inspectBackup(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                       [[maybe_unused]] QStringList xpubs,
                       [[maybe_unused]] QByteArray mnemonic) override {
        emit error(request_id, refusal());
    }

    void restore(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                 [[maybe_unused]] QStringList xpubs, [[maybe_unused]] QByteArray mnemonic,
                 [[maybe_unused]] QString name,
                 [[maybe_unused]] backend::Network network) override {
        emit error(request_id, refusal());
    }

private:
    QString m_account;
};

class Q_DECL_HIDDEN FixtureBlockchain final : public FixtureService<backend::IBlockchain> {
    Q_OBJECT
    Q_INTERFACES(backend::IBlockchain)

public:
    FixtureBlockchain(std::optional<backend::AccountWatch> watch, QObject *parent)
        : FixtureService(parent),
          m_watch(std::move(watch)) {
    }

    // The tip the "fixtureTip" property holds, none while it is unset.
    [[nodiscard]] auto state() const -> backend::BlockchainState override {
        auto tip = property("fixtureTip");
        if (!tip.isValid()) {
            return {};
        }
        return {.tip = tip.toULongLong()};
    }

    [[nodiscard]] auto coins() const -> QList<backend::Coin> override {
        return {};
    }

    [[nodiscard]] auto balance() const -> backend::Balance override {
        return {};
    }

    [[nodiscard]] auto history() const -> QList<backend::Transaction> override {
        return {backend::Transaction{.txid = pluginId(), .label = pluginId()}};
    }

    [[nodiscard]] auto transactionDetails([[maybe_unused]] const QString &txid) const
        -> std::optional<backend::TransactionDetails> override {
        return std::nullopt;
    }

    // One coinbase output per watched descriptor, at index 0 of its first branch, its
    // script this plugin's id. Once a test sets "fixtureOutputs", a list of maps of
    // outpoint, amount, script, keychain, index and height, those outputs of descriptor 0.
    [[nodiscard]] auto ownedOutputs() const -> QList<backend::OwnedOutput> override {
        QList<backend::OwnedOutput> outputs;
        if (property("fixtureOutputs").isValid()) {
            for (const auto &entry : property("fixtureOutputs").toList()) {
                auto output = entry.toMap();
                outputs.append(
                    {.outpoint = output.value(QString::fromLatin1("outpoint")).toString(),
                     .amount = output.value(QString::fromLatin1("amount")).toULongLong(),
                     .scriptPubKey = output.value(QString::fromLatin1("script")).toByteArray(),
                     .keychain = output.value(QString::fromLatin1("keychain")).toUInt(),
                     .index = output.value(QString::fromLatin1("index")).toUInt(),
                     .height = output.value(QString::fromLatin1("height")).toULongLong()});
            }
            return outputs;
        }
        for (quint32 descriptor = 0; descriptor < watchedCount(); ++descriptor) {
            outputs.append(
                {.outpoint = QString::fromLatin1("%1:%2").arg(pluginId()).arg(descriptor),
                 .amount = OUTPUT_SATS,
                 .scriptPubKey = pluginId().toLatin1(),
                 .descriptor = descriptor,
                 .height = m_watch->birthdayHeight,
                 .coinbase = true});
        }
        return outputs;
    }

    // Index 0 of each watched descriptor's first branch, or once a test sets "fixtureUsed",
    // a list of maps of keychain and last, those branches of descriptor 0.
    [[nodiscard]] auto usedIndexes() const -> QList<backend::UsedIndex> override {
        QList<backend::UsedIndex> used;
        if (property("fixtureUsed").isValid()) {
            for (const auto &entry : property("fixtureUsed").toList()) {
                auto branch = entry.toMap();
                used.append({.keychain = branch.value(QString::fromLatin1("keychain")).toUInt(),
                             .last = branch.value(QString::fromLatin1("last")).toUInt()});
            }
            return used;
        }
        for (quint32 descriptor = 0; descriptor < watchedCount(); ++descriptor) {
            used.append({.descriptor = descriptor, .last = 0});
        }
        return used;
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

    // Records the window in the "revealed" property as descriptor/keychain/index; a
    // descriptor not watched is refused.
    void reveal(backend::RequestId request_id, quint32 descriptor, quint32 keychain,
                quint32 index) override {
        if (descriptor >= watchedCount()) {
            emit error(request_id, refusal());
            return;
        }
        setProperty("revealed",
                    QString::fromLatin1("%1/%2/%3").arg(descriptor).arg(keychain).arg(index));
        emit completed(request_id);
    }

protected:
    [[nodiscard]] auto watchedCount() const -> quint32 {
        return m_watch.has_value() ? static_cast<quint32>(m_watch->descriptors.size()) : 0;
    }

private:
    std::optional<backend::AccountWatch> m_watch;
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
                .endpoints = {backend::Endpoint{.id = pluginId(), .url = pluginId()}}};
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
        QMap<QString, QString> meta{
            {QString::fromLatin1("id"), pluginId() + QString::fromLatin1(".module")}};
#ifdef SILENT_SERVICE_CHAIN_FIXTURE
        meta.insert(QString::fromLatin1(backend::WATCH_META),
                    QString::fromLatin1(backend::WATCH_DESCRIPTORS));
#endif
        return meta;
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
                               backend::ServiceKind kind,
                               const std::optional<backend::AccountWatch> &watch)
        -> backend::IService * override {
        if (!supplies(kind)) {
            return nullptr;
        }
        if (watch.has_value()) {
            recordWatch(*watch);
        }
        backend::IService *service = nullptr;
        switch (kind) {
        case backend::ServiceKind::Account:
            service = new FixtureAccount(account, this);
            break;
        case backend::ServiceKind::Blockchain:
            service = new FixtureBlockchain(watch, this);
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

protected:
    // "watches" counts the services asked with a watch; an unset birthday is an invalid
    // property.
    void recordWatch(const backend::AccountWatch &watch) {
        setProperty("watches", ++m_watches);
        setProperty("watchDescriptors", watch.descriptors);
        setProperty("watchBirthdayHeight", watch.birthdayHeight.has_value()
                                               ? QVariant::fromValue(*watch.birthdayHeight)
                                               : QVariant());
        setProperty("watchBirthdayTime", watch.birthdayTime.has_value()
                                             ? QVariant::fromValue(*watch.birthdayTime)
                                             : QVariant());
        setProperty("watchGapLimit", watch.gapLimit);
    }

private:
    int m_outstanding = 0;
    int m_watches = 0;
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
