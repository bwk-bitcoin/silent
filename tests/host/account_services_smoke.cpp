// Account UI service assembly: one provider, split providers (two external plugins, a
// built-in module and the host's explicit unsupported implementation), refusals that keep
// nothing, a supplying plugin retained until the assembly using it is deleted and every
// service it was handed back is destroyed, and the account shell over two plugins.

#include "AppController.h"
#include "PluginBundleFixture.h"
#include "account/AccountShell.h"
#include "account/SendSigning.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "host/ServiceAssembly.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/provider.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "interfaces/module.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonValue>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QWidget>
#include <Qontrol>
#include <common.h>
#include <memory>
#include <optional>
#include <silent.h>

namespace smoke {
namespace {
constexpr int TIMEOUT_MS = 10000;
// How long a retained plugin is watched not to stop.
constexpr int RETAINED_MS = 500;

const QString FULL = QStringLiteral("services.full");
const QString CHAIN = QStringLiteral("services.chain");
const QString BUILTIN = QStringLiteral("services.builtin");
const QString ACCOUNT = QStringLiteral("account-1");

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("account services: %s", message);
    }
}

template <typename Predicate> void waitFor(Predicate done, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(done(), message);
}

void spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}

// A built-in provider supplying the settings service only.
class BuiltinSettings final : public backend::IBackendSettings {
    Q_OBJECT
    Q_INTERFACES(backend::IBackendSettings)

public:
    using IBackendSettings::IBackendSettings;

    [[nodiscard]] auto settings() const -> backend::Settings override {
        return {.endpoints = {backend::Endpoint{.id = BUILTIN}}};
    }

    void raw(backend::RequestId request_id, [[maybe_unused]] QString operation,
             [[maybe_unused]] QByteArray payload) override {
        emit error(request_id, BUILTIN);
    }

    void value(backend::RequestId request_id, [[maybe_unused]] QString operation,
               [[maybe_unused]] QJsonValue payload) override {
        emit error(request_id, BUILTIN);
    }

    void testEndpoint(backend::RequestId request_id,
                      [[maybe_unused]] backend::Endpoint endpoint) override {
        emit endpointTested(request_id);
    }

    void save(backend::RequestId request_id, [[maybe_unused]] backend::Settings settings) override {
        emit saved(request_id);
    }
};

class BuiltinProvider final : public IModule, public backend::IServiceProvider {
    Q_OBJECT
    Q_INTERFACES(backend::IServiceProvider)

public:
    int outstanding = 0;
    // Set: the next settings request disables this plugin first, as a user could while
    // a provider spins a nested event loop mid-assembly.
    PluginRegistry *disable_registry = nullptr;
    QString disable_plugin;
    bool disabled = false;

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), BUILTIN + QStringLiteral(".module")}};
    }

    auto list() -> ReqId override {
        return 0;
    }

    auto autoStart() -> ReqId override {
        return 0;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }

    [[nodiscard]] auto service([[maybe_unused]] const QString &account,
                               [[maybe_unused]] backend::Network network, backend::ServiceKind kind)
        -> backend::IService * override {
        if (kind != backend::ServiceKind::Settings) {
            return nullptr;
        }
        if (disable_registry != nullptr) {
            disabled = disable_registry->setEnabled(disable_plugin, false);
            disable_registry = nullptr;
        }
        ++outstanding;
        return new BuiltinSettings(this);
    }

    void releaseService(backend::IService *service) override {
        --outstanding;
        service->deleteLater();
    }
};

class BuiltinPlugin final : public IPlugin {
public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), BUILTIN}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {m_module.get()};
    }

    [[nodiscard]] auto module() const -> BuiltinProvider * {
        return m_module.get();
    }

private:
    std::unique_ptr<BuiltinProvider> m_module = std::make_unique<BuiltinProvider>();
};

class Replies final : public QObject {
    Q_OBJECT

public:
    backend::RequestId errorRequest = 0;
    backend::RequestId valueRequest = 0;
    QJsonValue value;
    int retired = 0;
    bool stopFinished = false;
    QString stopError;

public slots:

    void onError(backend::RequestId request_id, [[maybe_unused]] const QString &message) {
        errorRequest = request_id;
    }

    void onValueResult(backend::RequestId request_id, const QJsonValue &payload) {
        valueRequest = request_id;
        value = payload;
    }

    void onRetired() {
        ++retired;
    }

    void onStopFinished([[maybe_unused]] const QString &id, [[maybe_unused]] bool removed,
                        const QString &error) {
        stopFinished = true;
        stopError = error;
    }
};

auto fixtureModule(PluginRegistry *registry, const QString &plugin_id) -> IModule * {
    auto module_id = plugin_id + QStringLiteral(".module");
    for (auto *plugin : registry->plugins()) {
        for (auto *module : plugin->modules()) {
            if (module->meta().value(QStringLiteral("id")) == module_id) {
                return module;
            }
        }
    }
    qFatal("account services: module %s is not loaded", qPrintable(module_id));
}

auto outstanding(IModule *module) -> int {
    return module->property("outstanding").toInt();
}

void enableFixture(PluginRegistry *registry, const QString &id, const char *path) {
    QFile library(QString::fromLocal8Bit(path));
    require(library.open(QIODevice::ReadOnly), "the provider fixture library opens");
    require(installBundle(id, id, library.readAll()).is_ok, "the provider fixture installs");
    registry->rescanDiscoveredPlugins();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin scan completes");
    require(registry->setEnabled(id, true), "the provider fixture enable is admitted");
    waitFor([registry,
             &id] { return !registry->pluginEnablePending(id) && !registry->pluginStateLoading(); },
            "the provider fixture enable completes");
    require(registry->isPluginEnabled(id), "the provider fixture is enabled");
}

void requireSupplier(const host::ServiceAssembly &assembly, backend::ServiceKind kind,
                     const QString &plugin_id, bool external, const char *message) {
    auto caller = assembly.supplier(kind);
    require(caller.pluginId == plugin_id && caller.external == external &&
                caller.moduleId ==
                    (plugin_id.isEmpty() ? QString() : plugin_id + QStringLiteral(".module")),
            message);
}

void oneProvider(HostImpl *host, IModule *full) {
    auto result = host->assembleServices({.account = ACCOUNT,
                                          .network = backend::Network::Regtest,
                                          .account_backend = full,
                                          .blockchain = full,
                                          .transaction = full,
                                          .labels = full,
                                          .settings = full});
    require(result.assembly != nullptr && result.error.isEmpty(),
            "one provider supplies a complete assembly");
    auto &assembly = *result.assembly;
    require(assembly.accountBackend() != nullptr && assembly.blockchain() != nullptr &&
                assembly.transaction() != nullptr && assembly.labels() != nullptr &&
                assembly.settings() != nullptr,
            "every required service is present");
    for (auto kind : {backend::ServiceKind::Account, backend::ServiceKind::Blockchain,
                      backend::ServiceKind::Transaction, backend::ServiceKind::Labels,
                      backend::ServiceKind::Settings}) {
        requireSupplier(assembly, kind, FULL, true, "each service is attributed to its plugin");
    }
    require(assembly.accountBackend()->id() == ACCOUNT &&
                assembly.accountBackend()->name() == FULL &&
                assembly.blockchain()->history().first().txid == FULL &&
                assembly.labels()->label({}) == FULL &&
                assembly.settings()->settings().endpoints.first().id == FULL &&
                assembly.transaction()->craft({}).completion.complete,
            "every getter reaches the plugin's implementation");
    Replies replies;
    QObject::connect(assembly.blockchain(), &backend::IService::valueResult, &replies,
                     &Replies::onValueResult, qontrol::UNIQUE);
    assembly.blockchain()->value(3, QStringLiteral("echo"), QJsonValue(QStringLiteral("x")));
    require(replies.valueRequest == 3 && replies.value.toString() == QStringLiteral("x"),
            "the JSON extension reaches the plugin and answers inline");
    require(outstanding(full) == 5, "the provider handed out five services");
    result.assembly.reset();
    require(outstanding(full) == 0, "deleting the assembly hands every service back");
}

auto splitProviders(HostImpl *host, IModule *full, IModule *chain, IModule *builtin)
    -> std::unique_ptr<host::ServiceAssembly> {
    auto result = host->assembleServices({.account = ACCOUNT,
                                          .network = backend::Network::Regtest,
                                          .account_backend = full,
                                          .blockchain = chain,
                                          .transaction = host::Unsupported{},
                                          .labels = chain,
                                          .settings = builtin});
    require(result.assembly != nullptr && result.error.isEmpty(),
            "independent providers supply one assembly");
    auto &assembly = *result.assembly;
    requireSupplier(assembly, backend::ServiceKind::Account, FULL, true,
                    "the account comes from the full plugin");
    requireSupplier(assembly, backend::ServiceKind::Blockchain, CHAIN, true,
                    "the blockchain comes from the chain plugin");
    requireSupplier(assembly, backend::ServiceKind::Labels, CHAIN, true,
                    "the labels come from the chain plugin");
    requireSupplier(assembly, backend::ServiceKind::Settings, BUILTIN, false,
                    "the settings come from the built-in module");
    requireSupplier(assembly, backend::ServiceKind::Transaction, QString(), false,
                    "the unsupported transaction service has no supplier");
    require(assembly.accountBackend()->name() == FULL &&
                assembly.blockchain()->history().first().txid == CHAIN &&
                assembly.labels()->label({}) == CHAIN &&
                assembly.settings()->settings().endpoints.first().id == BUILTIN,
            "each getter reaches its own provider");
    auto *transaction = assembly.transaction();
    require(transaction != nullptr, "the unsupported transaction service is present");
    auto crafted = transaction->craft({});
    require(!crafted.completion.complete && !crafted.completion.error.isEmpty() &&
                !transaction->finalize({}).has_value(),
            "the unsupported transaction service answers empty data and an explicit error");
    Replies replies;
    QObject::connect(transaction, &backend::IService::error, &replies, &Replies::onError,
                     qontrol::UNIQUE);
    transaction->selectCoins(5, {});
    require(replies.errorRequest == 5, "an unsupported request answers error() inline");
    transaction->value(6, QStringLiteral("echo"), QJsonValue());
    require(replies.errorRequest == 6, "an unsupported extension answers error() inline");
    require(outstanding(full) == 1 && outstanding(chain) == 2,
            "each plugin handed out only what it supplies");
    return std::move(result.assembly);
}

void refusals(HostImpl *host, IModule *full, IModule *chain) {
    auto missing = host->assembleServices({.account = ACCOUNT,
                                           .network = backend::Network::Regtest,
                                           .account_backend = full,
                                           .blockchain = full,
                                           .transaction = full,
                                           .labels = full,
                                           .settings = chain});
    require(missing.assembly == nullptr && missing.error.contains(QStringLiteral("settings")),
            "a provider that does not supply its service is refused, never substituted");
    auto network = host->assembleServices({.account = ACCOUNT,
                                           .network = backend::Network::Signet,
                                           .account_backend = full,
                                           .blockchain = full,
                                           .transaction = full,
                                           .labels = full,
                                           .settings = full});
    require(network.assembly == nullptr && !network.error.isEmpty(),
            "an account service of another network is refused");
    auto inactive = host->assembleServices({.account = ACCOUNT,
                                            .network = backend::Network::Regtest,
                                            .account_backend = static_cast<IModule *>(nullptr),
                                            .blockchain = full,
                                            .transaction = full,
                                            .labels = full,
                                            .settings = full});
    require(inactive.assembly == nullptr && !inactive.error.isEmpty(),
            "no provider module is refused, not taken as unsupported");
    auto unset = host->assembleServices({.account = ACCOUNT,
                                         .network = backend::Network::Regtest,
                                         .account_backend = full,
                                         .blockchain = full,
                                         .transaction = full,
                                         .labels = full});
    require(unset.assembly == nullptr && unset.error.contains(QStringLiteral("settings")),
            "an unset source is refused, not taken as unsupported");
    require(outstanding(full) == 1 && outstanding(chain) == 2,
            "refused assemblies hand back every service they were given");
}

// The chain plugin stops only once the assembly using its services is gone; the other
// providers of that assembly keep running.
void retention(HostImpl *host, PluginRegistry *registry,
               std::unique_ptr<host::ServiceAssembly> split, IModule *full, IModule *chain,
               BuiltinProvider *builtin) {
    Replies replies;
    QObject::connect(split.get(), &host::ServiceAssembly::retired, &replies, &Replies::onRetired,
                     qontrol::UNIQUE);
    QObject::connect(registry, &PluginRegistry::pluginStopFinished, &replies,
                     &Replies::onStopFinished, qontrol::UNIQUE);
    QPointer<IModule> chainModule(chain);
    builtin->disable_registry = registry;
    builtin->disable_plugin = CHAIN;
    auto during = host->assembleServices({.account = ACCOUNT,
                                          .network = backend::Network::Regtest,
                                          .account_backend = full,
                                          .blockchain = chain,
                                          .transaction = full,
                                          .labels = full,
                                          .settings = builtin});
    require(builtin->disabled, "the chain plugin disable is admitted mid-assembly");
    require(during.assembly == nullptr && during.error.contains(QStringLiteral("stopped")),
            "an assembly whose supplier starts stopping meanwhile is refused");
    require(outstanding(chain) == 2 && outstanding(full) == 1 && builtin->outstanding == 1,
            "the refused assembly handed back what it was given");
    require(replies.retired == 1, "a supplier that starts stopping retires its assemblies");
    auto late = host->assembleServices({.account = ACCOUNT,
                                        .network = backend::Network::Regtest,
                                        .account_backend = full,
                                        .blockchain = chain,
                                        .transaction = full,
                                        .labels = full,
                                        .settings = full});
    require(late.assembly == nullptr && outstanding(full) == 1,
            "a stopping provider supplies no new assembly");
    spin(RETAINED_MS);
    require(registry->pluginStopPending(CHAIN) && !replies.stopFinished && chainModule != nullptr &&
                outstanding(chain) == 2,
            "the chain plugin stays loaded while its services are assembled");
    QPointer<QObject> held = split->blockchain();
    split.release()->deleteLater();
    spin(RETAINED_MS);
    require(outstanding(chain) == 2 && registry->pluginStopPending(CHAIN),
            "a deferred deletion still holds the services");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(outstanding(chain) == 0 && held != nullptr,
            "the chain provider holds the services it was handed back");
    spin(RETAINED_MS);
    require(held != nullptr && registry->pluginStopPending(CHAIN) && chainModule != nullptr &&
                !replies.stopFinished,
            "the chain plugin stays loaded while a service it was handed back lives");
    waitFor([&replies] { return replies.stopFinished; }, "the chain plugin stop finishes");
    require(replies.stopError.isEmpty(), "the chain plugin stops cleanly once released");
    waitFor([registry] { return !registry->pluginStopPending(CHAIN); },
            "the chain plugin stop settles");
    require(chainModule == nullptr, "the chain plugin is unloaded");
    require(outstanding(full) == 0 && builtin->outstanding == 0,
            "the other providers got their services back");
    require(registry->isPluginEnabled(FULL), "the other providers are not stopped");
}

// A signing step that signs nothing: Send only needs one.
class IdleSigning final : public account::SendSigning {
public:
    auto actions() -> QWidget * override {
        return m_actions;
    }

    auto coinsWidget() -> QWidget * override {
        return nullptr;
    }

    void setTransaction([[maybe_unused]] const std::optional<backend::TransactionTemplate> &tx,
                        [[maybe_unused]] const QStringList &outpoints) override {
    }

    [[nodiscard]] auto estimatesFee() const -> bool override {
        return false;
    }

private:
    QWidget *m_actions = new QWidget(this);
};

// The account shell takes each service from its own plugin, parents none of them, builds
// its panels over them, and drops the panels, then the assembly, when a supplier stops.
void shellRetirement(PluginRegistry *registry, IModule *full) {
    require(registry->setEnabled(CHAIN, true), "the chain plugin enable is admitted again");
    waitFor(
        [registry] {
            return !registry->pluginEnablePending(CHAIN) && !registry->pluginStateLoading();
        },
        "the chain plugin enable completes");
    require(registry->isPluginEnabled(CHAIN), "the chain plugin is enabled again");
    auto *chain = fixtureModule(registry, CHAIN);
    QWidget page;
    account::AccountShell shell(account::accountMenuEntries(), new QWidget, &page);
    host::AssemblyRequest request{.account = ACCOUNT,
                                  .network = backend::Network::Regtest,
                                  .account_backend = full,
                                  .blockchain = chain,
                                  .transaction = full,
                                  .labels = chain,
                                  .settings = full};
    require(shell.assemble(request), "the shell assembles services of two plugins");
    auto services = shell.services();
    require(services.account->name() == FULL &&
                services.blockchain->history().first().txid == CHAIN &&
                services.labels->label({}) == CHAIN &&
                services.settings->settings().endpoints.first().id == FULL,
            "each service the shell holds comes from the plugin the request names");
    require(services.account->parent() == full && services.blockchain->parent() == chain,
            "the shell parents no service: each stays its provider's");
    require(outstanding(full) == 3 && outstanding(chain) == 2, "five services handed out");
    shell.openSettings(nullptr, false);
    shell.openFunds(new IdleSigning, full);
    for (const auto *name : {"history", "coins", "send", "receive", "settings"}) {
        require(shell.screen(QString::fromLatin1(name)).has_value(),
                "the shell builds every shared panel over them");
    }
    page.show();
    shell.showPanel(QStringLiteral("coins"));
    QPointer<qontrol::Screen> coins = shell.screen(QStringLiteral("coins")).value();
    require(coins->isVisible() && !shell.screen(QStringLiteral("history")).value()->isVisible(),
            "the shell shows the panel asked for");

    Replies replies;
    QObject::connect(registry, &PluginRegistry::pluginStopFinished, &replies,
                     &Replies::onStopFinished, qontrol::UNIQUE);
    require(registry->setEnabled(CHAIN, false), "the chain plugin disable is admitted");
    require(coins == nullptr && !shell.screen(QStringLiteral("settings")).has_value(),
            "a stopping supplier takes every panel down at once");
    require(!shell.assemble(request), "a retired shell assembles nothing again");
    require(outstanding(full) == 3 && registry->pluginStopPending(CHAIN),
            "the shell's assembly holds its services until it is deleted");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(outstanding(full) == 0, "the deleted assembly hands every service back");
    waitFor([&replies] { return replies.stopFinished; }, "the chain plugin stop finishes");
    require(replies.stopError.isEmpty(), "the chain plugin stops cleanly");
}

} // namespace

void accountServicesSmoke() {
    auto *controller = AppController::get();
    auto builtinPlugin = std::make_unique<BuiltinPlugin>();
    auto *builtin = builtinPlugin->module();
    controller->registerBuiltinPlugin(std::move(builtinPlugin));
    auto *registry = controller->pluginRegistry();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin state loads");
    enableFixture(registry, FULL, SILENT_SERVICE_FULL_FIXTURE);
    enableFixture(registry, CHAIN, SILENT_SERVICE_CHAIN_FIXTURE);
    auto *full = fixtureModule(registry, FULL);
    auto *chain = fixtureModule(registry, CHAIN);
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the host is the real host");

    oneProvider(host, full);
    auto split = splitProviders(host, full, chain, builtin);
    require(builtin->outstanding == 1, "the built-in provider handed out its service");
    refusals(host, full, chain);
    retention(host, registry, std::move(split), full, chain, builtin);
    shellRetirement(registry, full);
}

} // namespace smoke

#include "account_services_smoke.moc"
