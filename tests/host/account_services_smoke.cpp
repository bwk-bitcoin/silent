// Account UI service assembly: one provider, split providers (two external plugins, a
// built-in module and the host's explicit unsupported implementation), the account watch
// handed to the blockchain provider only and its outputs, indexes and reveal, refusals that
// keep nothing, a supplying plugin retained until the assembly using it is deleted and every
// service it was handed back is destroyed, the payloads it handed out outliving its unload,
// and the account shell over one plugin and over two, its panels showing each one's data.
// Apart, a Multikey account's blockchain backend: the plugins its Settings offers, its
// services assembled over the chosen one with the account's watch, a created account's
// birthday in it, the rescan from a height Settings offers with a plugin chosen alone, and a
// chosen backend that is not installed.

#include "AppController.h"
#include "MainWindow.h"
#include "PluginBundleFixture.h"
#include "account/AccountModule.h"
#include "account/AccountShell.h"
#include "account/PsbtTransfer.h"
#include "account/RequestId.h"
#include "account/SendSigning.h"
#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "catalog/form/LabelCell.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/inputs/ComboBox.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "host/ServiceAssembly.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/provider.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "interfaces/keys.h"
#include "interfaces/module.h"
#include "modules/multikey/MusigAccountController.h"
#include "modules/multikey/MusigAccountWidget.h"
#include "modules/multikey/MusigPlugin.h"
#include "modules/multikey/SignModal.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <memory>
#include <optional>
#include <silent.h>
#include <utility>

#define private public
#include "catalog/StatusBarItem.h"
#include "modules/multikey/MusigStatusBar.h"
#include "modules/multikey/SettingsExtras.h"
#undef private

namespace smoke {
namespace {
constexpr int TIMEOUT_MS = 10000;
// How long a retained plugin is watched not to stop.
constexpr int RETAINED_MS = 500;

const QString FULL = QStringLiteral("services.full");
const QString CHAIN = QStringLiteral("services.chain");
const QString BUILTIN = QStringLiteral("services.builtin");
const QString ACCOUNT = QStringLiteral("account-1");
const QString MULTIPATH_DESCRIPTOR = QStringLiteral(
    "wpkh(tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1Jzo"
    "QAzFRHfvNLEcqV7KxUHZVYa/<0;1>/*)#tyvpyexd");
const QString SINGLE_DESCRIPTOR = QStringLiteral(
    "tr(tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQ"
    "AzFRHfvNLEcqV7KxUHZVYa/2/*)#uw7h9w88");
constexpr quint64 BIRTHDAY_HEIGHT = 840000;
constexpr qint64 BIRTHDAY_TIME = 1713571767;
constexpr quint32 GAP_LIMIT = 50;
// Past the gap limit.
constexpr quint32 REVEALED_INDEX = GAP_LIMIT + 30;
// What each output of the fixture blockchain pays, in satoshis.
constexpr quint64 OUTPUT_SATS = 50000;
const QString MULTIKEY = QStringLiteral("services-multikey");
// A Multikey account created as the creator does, recording its birthday.
const QString CREATED_MULTIKEY = QStringLiteral("services-multikey-created");
// A Multikey account restored from a backup telling its birthday height and time.
const QString RESTORED_MULTIKEY = QStringLiteral("services-multikey-restored");
constexpr quint64 RESTORED_BIRTHDAY_HEIGHT = 812345;
// The window size of a Settings shot, and how long it settles before it is taken.
constexpr int SHOT_WIDTH = 1440;
constexpr int SHOT_HEIGHT = 1000;
constexpr int SHOT_SETTLE_MS = 500;
constexpr qint64 RESTORED_BIRTHDAY_TIME = 1700000000;
// The height the rescan is asked from.
constexpr quint64 RESCAN_HEIGHT = 150;
// The chain fixture's module id: the backend the Multikey account chooses.
const QString CHAIN_MODULE = CHAIN + QStringLiteral(".module");
const char *const MULTIKEY_DESCRIPTOR =
    "tr(musig([4ba43603/87'/1'/0'/2]tpubDFGmrfdE8Bya4CPLde4Q54gxkY4KKJzzs6y5tGWxVEcrfmzZb17aCamv"
    "X9k1UwKPPHtd7u5WqyS5SiRwxBZvHhbCJumqwECQQycuAmYtfzZ,[8dfc9b34/87'/1'/0'/2]tpubDFWeBGy8cbQXd"
    "CA37ePK189L12ML8fZCuuG9EZtwybxBx2jrVRLrWwsXvRHvXuCS5Q6sRyLB92TsidhBVbwWoeTAxDsDC2w5vQmPVnL2v"
    "KB)/<0;1>/*,pk([56c4fac3/87'/1'/0'/2]tpubDE1s5mmJDMZQ9uwi2KHe99ioqqEpwwZbAmAWP1UK6Y6zHLTKQw"
    "hkGLeSsQi38VaBwzfWtUrSL7v4tD1DPmHKjSfXBRbXVYUVboM7idebk7T/<2;3>/*))";

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
                               [[maybe_unused]] backend::Network network, backend::ServiceKind kind,
                               [[maybe_unused]] const std::optional<backend::AccountWatch> &watch)
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
    backend::RequestId completedRequest = 0;
    backend::RequestId valueRequest = 0;
    QJsonValue value;
    QString valueOperation;
    QJsonValue valueEvent;
    backend::RequestId rawRequest = 0;
    QByteArray raw;
    QString rawOperation;
    QByteArray rawEvent;
    int retired = 0;
    bool stopFinished = false;
    QString stopError;

public slots:

    void onError(backend::RequestId request_id, [[maybe_unused]] const QString &message) {
        errorRequest = request_id;
    }

    void onCompleted(backend::RequestId request_id) {
        completedRequest = request_id;
    }

    void onValueResult(backend::RequestId request_id, const QJsonValue &payload) {
        valueRequest = request_id;
        value = payload;
    }

    void onValueEvent(const QString &operation, const QJsonValue &payload) {
        valueOperation = operation;
        valueEvent = payload;
    }

    void onRawResult(backend::RequestId request_id, const QByteArray &payload) {
        rawRequest = request_id;
        raw = payload;
    }

    void onRawEvent(const QString &operation, const QByteArray &payload) {
        rawOperation = operation;
        rawEvent = payload;
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

// Whether the library of `plugin_id` is mapped into this process (from its memfd).
auto libraryMapped(const QString &plugin_id) -> bool {
    QFile maps(QStringLiteral("/proc/self/maps"));
    require(maps.open(QIODevice::ReadOnly), "the process maps open");
    return maps.readAll().contains(QStringLiteral("lib%1.so").arg(plugin_id).toUtf8());
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

// `module` was asked `watches` services with a watch, the last one `watch`.
void requireWatch(const IModule *module, const backend::AccountWatch &watch, int watches,
                  const char *message) {
    auto height = module->property("watchBirthdayHeight");
    auto time = module->property("watchBirthdayTime");
    require(module->property("watches").toInt() == watches &&
                module->property("watchDescriptors").toStringList() == watch.descriptors &&
                (height.isValid() ? std::optional(height.toULongLong()) : std::nullopt) ==
                    watch.birthdayHeight &&
                (time.isValid() ? std::optional(time.toLongLong()) : std::nullopt) ==
                    watch.birthdayTime &&
                module->property("watchGapLimit").toUInt() == watch.gapLimit,
            message);
}

// The chain plugin got the watch of the split assembly for its blockchain only, and its
// blockchain reports the outputs and indexes of the watched descriptors and reveals.
void watchedChain(const host::ServiceAssembly &assembly, const IModule *chain,
                  const backend::AccountWatch &watch) {
    requireWatch(chain, watch, 1,
                 "the blockchain provider receives the watch intact, its labels none");
    auto *blockchain = assembly.blockchain();
    auto outputs = blockchain->ownedOutputs();
    require(outputs.size() == watch.descriptors.size(), "one output per watched descriptor");
    for (qsizetype descriptor = 0; descriptor < outputs.size(); ++descriptor) {
        const auto &output = outputs.at(descriptor);
        require(output.outpoint == QStringLiteral("%1:%2").arg(CHAIN).arg(descriptor) &&
                    output.amount == OUTPUT_SATS && output.scriptPubKey == CHAIN.toLatin1() &&
                    std::cmp_equal(output.descriptor, descriptor) && output.keychain == 0 &&
                    output.index == 0 && output.height == BIRTHDAY_HEIGHT && output.coinbase &&
                    output.spentBy.isEmpty(),
                "each owned output reaches the consumer as the provider reports it");
    }
    auto used = blockchain->usedIndexes();
    require(used.size() == watch.descriptors.size() && used.at(1).descriptor == 1 &&
                used.at(1).keychain == 0 && used.at(1).last == 0U,
            "the used indexes reach the consumer as the provider reports them");
    Replies replies;
    QObject::connect(blockchain, &backend::IBlockchain::completed, &replies, &Replies::onCompleted,
                     qontrol::UNIQUE);
    QObject::connect(blockchain, &backend::IService::error, &replies, &Replies::onError,
                     qontrol::UNIQUE);
    auto revealed = QStringLiteral("1/1/%1").arg(REVEALED_INDEX);
    blockchain->reveal(7, 1, 1, REVEALED_INDEX);
    require(replies.completedRequest == 7 &&
                blockchain->property("revealed").toString() == revealed,
            "a reveal reaches the provider and is acknowledged");
    blockchain->reveal(8, 2, 0, 1);
    require(replies.errorRequest == 8 && replies.completedRequest == 7 &&
                blockchain->property("revealed").toString() == revealed,
            "a reveal of a descriptor not watched is refused");
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
    backend::AccountWatch watch{.descriptors = {SINGLE_DESCRIPTOR}};
    auto result = host->assembleServices({.account = ACCOUNT,
                                          .network = backend::Network::Regtest,
                                          .account_backend = full,
                                          .blockchain = full,
                                          .transaction = full,
                                          .labels = full,
                                          .settings = full,
                                          .watch = watch});
    require(result.assembly != nullptr && result.error.isEmpty(),
            "one provider supplies a complete assembly");
    auto &assembly = *result.assembly;
    requireWatch(full, watch, 1,
                 "of five services, the blockchain alone is given the watch, its birthday unset "
                 "and its gap limit the default");
    require(assembly.blockchain()->ownedOutputs().size() == 1 &&
                !assembly.blockchain()->ownedOutputs().first().height.has_value(),
            "the blockchain watches the descriptor it was given");
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
    QObject::connect(assembly.blockchain(), &backend::IService::valueEvent, &replies,
                     &Replies::onValueEvent, qontrol::UNIQUE);
    QObject::connect(assembly.settings(), &backend::IService::rawResult, &replies,
                     &Replies::onRawResult, qontrol::UNIQUE);
    QObject::connect(assembly.settings(), &backend::IService::rawEvent, &replies,
                     &Replies::onRawEvent, qontrol::UNIQUE);
    assembly.blockchain()->value(3, QStringLiteral("echo"), QJsonValue(QStringLiteral("x")));
    require(replies.valueRequest == 3 && replies.value.toString() == QStringLiteral("x"),
            "the JSON extension reaches the plugin and answers inline");
    require(replies.valueOperation == QStringLiteral("echo") &&
                replies.valueEvent.toString() == QStringLiteral("x"),
            "a JSON event of the plugin reaches the consumer");
    assembly.settings()->raw(4, QStringLiteral("echo"), QByteArrayLiteral("bytes"));
    require(replies.rawRequest == 4 && replies.raw == QByteArrayLiteral("bytes"),
            "the byte extension reaches the plugin and answers inline");
    require(replies.rawOperation == QStringLiteral("echo") &&
                replies.rawEvent == QByteArrayLiteral("bytes"),
            "a byte event of the plugin reaches the consumer");
    require(outstanding(full) == 5, "the provider handed out five services");
    result.assembly.reset();
    require(outstanding(full) == 0, "deleting the assembly hands every service back");
}

auto splitProviders(HostImpl *host, IModule *full, IModule *chain, IModule *builtin)
    -> std::unique_ptr<host::ServiceAssembly> {
    backend::AccountWatch watch{.descriptors = {MULTIPATH_DESCRIPTOR, SINGLE_DESCRIPTOR},
                                .birthdayHeight = BIRTHDAY_HEIGHT,
                                .birthdayTime = BIRTHDAY_TIME,
                                .gapLimit = GAP_LIMIT};
    auto result = host->assembleServices({.account = ACCOUNT,
                                          .network = backend::Network::Regtest,
                                          .account_backend = full,
                                          .blockchain = chain,
                                          .transaction = host::Unsupported{},
                                          .labels = chain,
                                          .settings = builtin,
                                          .watch = watch});
    require(result.assembly != nullptr && result.error.isEmpty(),
            "independent providers supply one assembly");
    auto &assembly = *result.assembly;
    watchedChain(assembly, chain, watch);
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
    // Copies of what the chain plugin handed out, read again once it is unloaded.
    auto history = split->blockchain()->history();
    auto label = split->labels()->label({});
    require(libraryMapped(CHAIN), "the chain library is mapped while it runs");
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
    require(chainModule == nullptr && !libraryMapped(CHAIN), "the chain plugin is unloaded");
    require(history.size() == 1 && history.first().txid == CHAIN &&
                history.first().label == CHAIN && label == CHAIN,
            "what the chain plugin handed out outlives its unload");
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

// Builds every shared panel of `shell`, then checks History shows the history of the plugin
// `history_id` and Settings the endpoints of the plugin `settings_id`.
void openPanels(account::AccountShell &shell, IModule *caller, const QString &history_id,
                const QString &settings_id) {
    shell.openSettings(nullptr, false);
    shell.openFunds(new IdleSigning, caller);
    for (const auto *name : {"history", "coins", "send", "receive", "settings"}) {
        require(shell.screen(QString::fromLatin1(name)).has_value(),
                "the shell builds every shared panel over them");
    }
    // A table lays its rows out on the next event loop pass.
    QCoreApplication::processEvents();
    auto labels =
        shell.screen(QStringLiteral("history")).value()->findChildren<catalog::LabelCell *>();
    require(std::ranges::any_of(labels,
                                [&history_id](const catalog::LabelCell *cell) {
                                    return cell->label() == history_id;
                                }),
            "History shows the history of the plugin supplying the blockchain");
    auto fields = shell.screen(QStringLiteral("settings")).value()->findChildren<QLineEdit *>();
    require(std::ranges::any_of(
                fields,
                [&settings_id](const QLineEdit *field) { return field->text() == settings_id; }),
            "Settings shows the endpoints of the plugin supplying the settings");
}

// The account shell over one plugin supplying all five services.
void shellOneProvider(IModule *full) {
    {
        QWidget page;
        account::AccountShell shell(account::accountMenuEntries(), new QWidget, &page);
        require(shell.assemble({.account = ACCOUNT,
                                .network = backend::Network::Regtest,
                                .account_backend = full,
                                .blockchain = full,
                                .transaction = full,
                                .labels = full,
                                .settings = full}),
                "the shell assembles the services of one plugin");
        require(outstanding(full) == 5, "the plugin handed out five services");
        openPanels(shell, full, FULL, FULL);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(outstanding(full) == 0, "deleting the shell hands every service back");
}

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
    require(services.blockchain->ownedOutputs().isEmpty() &&
                services.blockchain->usedIndexes().isEmpty(),
            "a blockchain given no watch reports no owned output nor used index");
    require(services.account->parent() == full && services.blockchain->parent() == chain,
            "the shell parents no service: each stays its provider's");
    require(outstanding(full) == 3 && outstanding(chain) == 2, "five services handed out");
    openPanels(shell, full, CHAIN, FULL);
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

// The Settings extras of the open Multikey account of `widget`, once built.
auto backendExtras(MusigAccountWidget *widget) -> musig::SettingsExtras * {
    waitFor(
        [widget] { return widget->controller()->screen(QStringLiteral("settings")).has_value(); },
        "the Multikey Settings is built");
    auto *extras = widget->controller()
                       ->screen(QStringLiteral("settings"))
                       .value()
                       ->findChild<musig::SettingsExtras *>();
    require(extras != nullptr, "the Multikey Settings shows its extras");
    return extras;
}

// The module id of each entry of the backend picker, empty for Electrum.
auto backendChoices(const musig::SettingsExtras *extras) -> QStringList {
    QStringList choices;
    for (int index = 0; index < extras->m_backend_picker->count(); ++index) {
        choices.append(extras->m_backend_picker->itemData(index).toString());
    }
    return choices;
}

// The shell's blockchain, once assembled.
auto assembledBlockchain(MusigAccountWidget *widget) -> backend::IBlockchain * {
    return widget->shell()->services().blockchain;
}

// The account's blockchain is the host's unsupported implementation, which the assembly
// owns: neither Electrum nor a plugin's.
auto unsupportedBlockchain(MusigAccountWidget *widget) -> bool {
    auto *blockchain = assembledBlockchain(widget);
    return blockchain != nullptr &&
           qobject_cast<host::ServiceAssembly *>(blockchain->parent()) != nullptr;
}

// Settings shows the chosen backend `CHAIN_MODULE` as missing, by its id, and the account
// opens no funds panel.
void requireMissingBackend(MusigAccountWidget *widget, const char *message) {
    auto *extras = backendExtras(widget);
    require(widget->controller()->blockchainBackendMissing() && unsupportedBlockchain(widget) &&
                !widget->shell()->fundsOpen() &&
                widget->shell()
                    ->screen(QStringLiteral("settings"))
                    .value()
                    ->isVisibleTo(widget->shell()->widget()) &&
                !extras->m_backend_error->isHidden() &&
                extras->m_backend_error->text() == TR("musig-backend-missing").arg(CHAIN_MODULE) &&
                extras->m_backend_picker->currentData().toString() == CHAIN_MODULE &&
                backendChoices(extras) == QStringList({QString(), CHAIN_MODULE}) &&
                extras->m_rescan->isHidden(),
            message);
}

// Picks `module_id` in the backend picker as the user does, then waits for the services
// assembled over it.
void chooseBackend(MusigAccountWidget *widget, const QString &module_id) {
    auto *picker = backendExtras(widget)->m_backend_picker;
    auto index = picker->findData(module_id);
    require(index >= 0, "the backend picker lists the backend");
    picker->setCurrentIndex(index);
    emit picker->activated(index);
    require(!picker->isEnabled(), "the picker waits for the choice to save");
    // Both saves write the account's config: a Settings save waits for the backend's.
    require(!widget->controller()->endpointChangeRefusal().isEmpty(),
            "a Settings save is refused while the backend choice saves");
    waitFor(
        [widget, &module_id] {
            return widget->controller()->blockchainBackend() == module_id &&
                   assembledBlockchain(widget) != nullptr;
        },
        "the services are assembled again over the chosen backend");
}

// The chain plugin was asked its blockchain `watches` times, with the account's
// checksummed descriptor, no birthday and the default gap limit, and supplies it alone.
void requireChainBackend(MusigAccountWidget *widget, IModule *chain, int watches,
                         const char *message) {
    auto canonical = ::validate_musig_descriptor(rust::String(MULTIKEY_DESCRIPTOR));
    auto descriptor = QString::fromStdString(std::string(canonical.descriptor));
    require(canonical.is_ok && descriptor.contains(QLatin1Char('#')),
            "the account's descriptor has a checksum");
    requireWatch(chain, {.descriptors = {descriptor}}, watches, message);
    require(assembledBlockchain(widget)->parent() == chain && outstanding(chain) == 1 &&
                !widget->controller()->blockchainBackendMissing() &&
                backendExtras(widget)->m_backend_error->isHidden(),
            "the blockchain alone comes from the chain plugin");
}

auto savedBackend() -> QString {
    return QString::fromStdString(std::string(
        ::config_from_file(rust::String(MULTIKEY.toStdString()))->get_blockchain_backend()));
}

void deleteDeferred() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// Settings offers the rescan, its height empty: a Multikey account records no birthday
// height. Each rescan asked reaches the chain plugin's blockchain with the height entered,
// and Settings says it started, or shows the plugin's refusal.
void rescanFromHeight(MusigAccountWidget *widget) {
    auto *extras = backendExtras(widget);
    auto *chain = assembledBlockchain(widget);
    require(!extras->m_rescan->isHidden() && extras->m_rescan_input->input()->text().isEmpty() &&
                !extras->m_rescan_btn->isEnabled() && extras->m_rescan_status->isHidden(),
            "a plugin chosen, Settings offers a rescan from a height, empty until one is entered");
    extras->m_rescan_input->input()->setText(QString::number(RESCAN_HEIGHT));
    require(extras->m_rescan_btn->isEnabled(), "a height entered, the rescan can be asked");
    emit extras->m_rescan_btn->clicked();
    waitFor([extras] { return !extras->m_rescan_status->isHidden(); }, "the rescan is answered");
    require(chain->property("rescanned").toString() == QString::number(RESCAN_HEIGHT) &&
                extras->m_rescan_status->text() == TR("musig-rescan-started").arg(RESCAN_HEIGHT) &&
                extras->m_rescan_btn->isEnabled(),
            "the backend rescans from the height entered, and Settings says so");

    chain->setProperty("rescanned", QVariant());
    chain->setProperty("fixtureRefuseRescan", true);
    emit extras->m_rescan_btn->clicked();
    waitFor(
        [extras] {
            return extras->m_rescan_status->text() != TR("musig-rescan-started").arg(RESCAN_HEIGHT);
        },
        "the refused rescan is answered");
    require(!chain->property("rescanned").isValid() &&
                extras->m_rescan_status->text() ==
                    QStringLiteral("services.chain refuses this request") &&
                extras->m_rescan_btn->isEnabled(),
            "a rescan the backend refuses shows its refusal");
    chain->setProperty("fixtureRefuseRescan", QVariant());
}

// Saves the whole window to `path`, showing the Settings of the account of `widget`.
void captureSettings(MainWindow &window, MusigAccountWidget *widget, const QString &path) {
    window.resize(SHOT_WIDTH, SHOT_HEIGHT);
    window.show();
    window.showTab(widget);
    widget->controller()->settingsClicked();
    spin(SHOT_SETTLE_MS);
    require(window.grab().save(path), "the window showing Settings is saved");
}

// Opens the Multikey account `name` with the chain plugin as its backend, beside `opened`:
// its widget once its services are assembled.
auto openChainAccount(MainWindow &window, account::AccountModule *module, const QString &name,
                      MusigAccountWidget *opened) -> MusigAccountWidget * {
    require(::save_blockchain_backend(rust::String(name.toStdString()),
                                      rust::String(CHAIN_MODULE.toStdString())),
            "the account chooses the chain plugin");
    module->startInstance(name);
    MusigAccountWidget *widget = nullptr;
    waitFor(
        [&window, opened, &widget] {
            for (auto *candidate : window.findChildren<MusigAccountWidget *>()) {
                if (candidate != opened && candidate->shell() != nullptr &&
                    assembledBlockchain(candidate) != nullptr) {
                    widget = candidate;
                }
            }
            return widget != nullptr;
        },
        "the account's services are assembled");
    return widget;
}

// Closes the Multikey account `name` and deletes it.
void closeAndDelete(account::AccountModule *module, const QString &name) {
    Host::get()->removeInstance(name, module);
    waitFor(
        [module, name] {
            deleteDeferred();
            return !module->accountUnavailable(name);
        },
        "the account closes");
    require(::delete_musig_account(rust::String(name.toStdString())).empty(),
            "the account is deleted");
}

// The canonical MULTIKEY_DESCRIPTOR, as a watch holds it.
auto watchedDescriptor() -> QString {
    auto canonical = ::validate_musig_descriptor(rust::String(MULTIKEY_DESCRIPTOR));
    return QString::fromStdString(std::string(canonical.descriptor));
}

// A Multikey account created as the creator does, the chain plugin its backend, hands the
// plugin a watch carrying the time it was created at.
void createdAccountBirthday(MainWindow &window, account::AccountModule *module, IModule *chain,
                            MusigAccountWidget *opened, int watches) {
    auto before = QDateTime::currentSecsSinceEpoch();
    auto created = ::create_musig_account(
        MusigCreationRequest{.account_name = rust::String(CREATED_MULTIKEY.toStdString()),
                             .network = Network::Regtest,
                             .descriptor = rust::String(MULTIKEY_DESCRIPTOR),
                             .electrum_url = rust::String(""),
                             .verify_certificate = true});
    require(created.is_ok, "the Multikey account is created");
    auto birthday = static_cast<qint64>(
        ::config_from_file(rust::String(CREATED_MULTIKEY.toStdString()))->get_birthday_time());
    require(birthday >= before && birthday <= QDateTime::currentSecsSinceEpoch(),
            "the created account records when it was created");
    auto *widget = openChainAccount(window, module, CREATED_MULTIKEY, opened);
    requireWatch(chain, {.descriptors = {watchedDescriptor()}, .birthdayTime = birthday}, watches,
                 "the created account's watch carries its birthday time, no height");
    require(backendExtras(widget)->m_rescan_input->input()->text().isEmpty(),
            "with no birthday height the rescan field starts empty");
    closeAndDelete(module, CREATED_MULTIKEY);
}

// A Multikey account restored from a BIP139 backup with a block height and a timestamp, the
// chain plugin its backend, hands the plugin a watch carrying both, and its Settings rescan
// field starts at that height. With SILENT_RESCAN_SHOT set, the whole window showing that
// Settings is saved there.
void restoredAccountBirthday(MainWindow &window, account::AccountModule *module, IModule *chain,
                             MusigAccountWidget *opened, int watches) {
    auto document = QJsonDocument(QJsonObject{
        {QStringLiteral("network"), QStringLiteral("regtest")},
        {QStringLiteral("accounts"),
         QJsonArray{QJsonObject{
             {QStringLiteral("type"), QStringLiteral("bip_380")},
             {QStringLiteral("descriptor"), QString::fromLatin1(MULTIKEY_DESCRIPTOR)},
             {QStringLiteral("timestamp"), RESTORED_BIRTHDAY_TIME},
             {QStringLiteral("block_height"), static_cast<qint64>(RESTORED_BIRTHDAY_HEIGHT)}}}}});
    auto bytes = document.toJson();
    auto restored = ::restore_account_backup(
        rust::Str("musig"),
        rust::Slice<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                                        static_cast<std::size_t>(bytes.size())),
        {}, rust::String(""), rust::String(RESTORED_MULTIKEY.toStdString()), Network::Regtest);
    require(restored.is_ok, "the Multikey backup restores");
    auto *widget = openChainAccount(window, module, RESTORED_MULTIKEY, opened);
    requireWatch(chain,
                 {.descriptors = {watchedDescriptor()},
                  .birthdayHeight = RESTORED_BIRTHDAY_HEIGHT,
                  .birthdayTime = RESTORED_BIRTHDAY_TIME},
                 watches, "the restored account's watch carries its birthday height and time");
    auto *extras = backendExtras(widget);
    require(extras->m_rescan_input->input()->text() == QString::number(RESTORED_BIRTHDAY_HEIGHT) &&
                extras->m_rescan_btn->isEnabled(),
            "the rescan field starts at the birthday height, Rescan offered");
    auto shot = qEnvironmentVariable("SILENT_RESCAN_SHOT");
    if (!shot.isEmpty()) {
        captureSettings(window, widget, shot);
    }
    closeAndDelete(module, RESTORED_MULTIKEY);
}

// How long a probe of a server nothing listens on takes at most.
constexpr int PROBE_MS = 2000;
// A regtest address of no test account (BIP-173).
const QString EXTERNAL = QStringLiteral("bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080");
// What the backend's coin pays, in satoshis.
constexpr quint64 CHAIN_COIN_SATS = 100000;
// The backend's tip, and the height confirming its coin.
constexpr quint64 CHAIN_TIP = 200;
constexpr quint64 CHAIN_COIN_HEIGHT = 150;
// Receive index 1, the first receive address, is the backend's used one: with the default
// gap limit of 20 it watches up to index 21, and 22 needs a reveal.
constexpr quint32 FIRST_UNWATCHED = 22;
// The change index the backend reports used, past the account scanner's look-ahead of 20:
// the spend's change goes to the one after, which adoption must take as change.
constexpr quint32 CHAIN_USED_CHANGE = 60;
const QString CHAIN_COIN =
    QStringLiteral("1111111111111111111111111111111111111111111111111111111111111111:0");

// The scriptPubKey of the segwit address `address`: its witness version then its program,
// read from the bech32 data part without checking the checksum.
auto segwitScript(const QString &address) -> QByteArray {
    const QString charset = QStringLiteral("qpzry9x8gf2tvdw0s3jn54khce6mua7l");
    auto data = address.mid(address.lastIndexOf(QLatin1Char('1')) + 1).chopped(6);
    auto version = charset.indexOf(data.front());
    QByteArray program;
    int accumulator = 0;
    int bits = 0;
    for (auto character : data.mid(1)) {
        accumulator = (accumulator << 5) | static_cast<int>(charset.indexOf(character));
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            program.append(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    QByteArray script;
    script.append(static_cast<char>(version == 0 ? 0 : 0x50 + version));
    script.append(static_cast<char>(program.size()));
    script.append(program);
    return script;
}

// The password sealing the chain spend's hot key.
const char *const CHAIN_KEY_PASSWORD = "chain spend password";

// The chain spend's hot key, of the BIP39 test mnemonic: its material for signing and the
// account policy, MULTIKEY_DESCRIPTOR's musig(A,B) key path and a leaf of that key alone.
struct ChainHotKey {
    QString fingerprint;
    QString descriptor;
    std::optional<rust::Box<HotMaterial>> material;
};

auto chainHotKey() -> ChainHotKey {
    auto key = std::make_shared<ChainHotKey>();
    // hot_seal and hot_xpub block on Argon2id.
    auto *worker = QThread::create([key] {
        auto sealed = ::hot_seal("abandon abandon abandon abandon abandon abandon abandon abandon "
                                 "abandon abandon abandon about",
                                 CHAIN_KEY_PASSWORD, Network::Regtest);
        require(sealed->is_ok(), "the chain spend's hot key seals");
        key->fingerprint = QString::fromStdString(std::string(sealed->fingerprint()));
        auto keys = ::new_keys_module(Network::Regtest);
        require(keys->commit_hot_key(*sealed, "Chain spend key").is_ok,
                "the chain spend's hot key commits");
        key->material.emplace(keys->hot_material(sealed->fingerprint()));
        auto xpub = ::hot_xpub(**key->material, CHAIN_KEY_PASSWORD, "m/87'/1'/0'/2");
        require(xpub.is_ok, "the chain spend's hot key derives its policy xpub");
        auto musigKeyPath =
            QString::fromLatin1(MULTIKEY_DESCRIPTOR).section(QStringLiteral(",pk("), 0, 0);
        key->descriptor = QStringLiteral("%1,pk([%2/87'/1'/0'/2]%3/<2;3>/*))")
                              .arg(musigKeyPath, key->fingerprint,
                                   QString::fromStdString(std::string(xpub.xpub)));
        // The key signs only for the descriptors registered with it.
        require(
            keys->register_descriptor(sealed->fingerprint(), key->descriptor.toStdString()).is_ok,
            "the account policy registers with the chain spend's hot key");
        key->material.emplace(keys->hot_material(sealed->fingerprint()));
    });
    worker->start();
    worker->wait();
    delete worker;
    return std::move(*key);
}

// `psbt_v2_hex` signed by `key`, off the GUI thread: hot_sign blocks on Argon2id.
auto hotSigned(const ChainHotKey &key, const QString &psbt_v2_hex) -> QString {
    auto result = std::make_shared<SignResult>();
    auto *worker = QThread::create([&key, result, hex = psbt_v2_hex.toStdString()] {
        *result = ::hot_sign(**key.material, CHAIN_KEY_PASSWORD, hex, {});
    });
    worker->start();
    worker->wait();
    delete worker;
    if (!result->is_ok) {
        qFatal("account services: the hot key signs the adopted spend: %s",
               std::string(result->error).c_str());
    }
    return QString::fromStdString(std::string(result->psbt_v2_hex));
}

// How often SignModalDriver looks for the modal the adoption opens.
constexpr int MODAL_POLL_MS = 10;

// Signs the spend the account adopted while the Sign modal the adoption opens runs, its own
// loop holding the GUI thread until it closes: imports `key`'s signature of the PSBT in
// flight, waits for the account to finalize it, then closes the modal.
class SignModalDriver final : public QObject {
    Q_OBJECT

public:
    SignModalDriver(MusigAccountController *account, const ChainHotKey &key)
        : m_account(account),
          m_key(key) {
        connect(&m_timer, &QTimer::timeout, this, &SignModalDriver::onPoll, qontrol::UNIQUE);
        connect(account, &MusigAccountController::psbtImported, this,
                &SignModalDriver::onPsbtImported, qontrol::UNIQUE);
        m_timer.start(MODAL_POLL_MS);
    }

    // The transaction the account finalized, empty until the modal closed.
    QString finalized;

public slots:

    void onPoll() {
        auto *active = QApplication::activeModalWidget();
        if (active == nullptr) {
            return;
        }
        m_timer.stop();
        auto *modal = qobject_cast<musig::SignModal *>(active);
        require(modal != nullptr, "the adoption opens the Sign modal, no error");
        m_account->importSavedPsbt(
            QByteArray::fromHex(hotSigned(m_key, m_account->psbtInFlight()).toLatin1()));
        waitFor([this] { return m_imported.has_value(); }, "the import answers");
        require(m_imported->is_ok && m_imported->merged,
                "the signed answer merges into the adopted spend");
        waitFor([this] { return !m_account->finalizedTx().isEmpty(); }, "the spend finalizes");
        finalized = m_account->finalizedTx();
        modal->reject();
    }

    void onPsbtImported(const SavedPsbtImport &result) {
        m_imported = result;
    }

private:
    MusigAccountController *m_account;
    const ChainHotKey &m_key;
    QTimer m_timer;
    std::optional<SavedPsbtImport> m_imported;
};

// The answers of the Multikey account's transaction and account services.
class ChainReplies final : public QObject {
    Q_OBJECT

public:
    std::optional<backend::CraftResult> selected;
    QList<backend::Address> addresses;
    QString failure;

public slots:

    void onCoinsSelected([[maybe_unused]] backend::RequestId request_id,
                         const backend::CraftResult &result) {
        selected = result;
    }

    void onAddressCreated([[maybe_unused]] backend::RequestId request_id,
                          const backend::Address &address) {
        addresses.append(address);
    }

    void onError([[maybe_unused]] backend::RequestId request_id, const QString &message) {
        failure = message;
    }
};

// The answers of the Multikey account's exports. Each export opens the modal handing the
// PSBT out, its own loop holding the GUI thread until it closes: closed once it runs, any
// other modal taken as a failure.
class ExportReplies final : public QObject {
    Q_OBJECT

public:
    ExportReplies() {
        connect(&m_timer, &QTimer::timeout, this, &ExportReplies::onPoll, qontrol::UNIQUE);
        m_timer.start(MODAL_POLL_MS);
    }

    int exported = 0;
    QString failure;

public slots:

    void onPoll() {
        auto *active = QApplication::activeModalWidget();
        if (active == nullptr) {
            return;
        }
        if (qobject_cast<account::ExportPsbt *>(active) == nullptr) {
            failure = QStringLiteral("the export shows another modal");
        }
        qobject_cast<QDialog *>(active)->reject();
    }

    void onExported([[maybe_unused]] const QByteArray &psbt) {
        ++exported;
    }

    void onExportFailed(const QString &message) {
        failure = message;
    }

private:
    QTimer m_timer;
};

// The label the account's status bar item shows.
auto statusLabel(MusigAccountWidget *widget) -> QString {
    auto *bar = widget->findChild<MusigStatusBar *>();
    require(bar != nullptr, "the Multikey account has its status bar");
    return bar->m_electrum_item->m_label->text();
}

// The backend reports one coin at receive index 1, the first receive address of
// `descriptor`, that index and change index CHAIN_USED_CHANGE used, and its tip.
void reportChainCoin(backend::IBlockchain *chain, const QString &descriptor) {
    auto first =
        ::musig_first_receive_address(rust::String(descriptor.toStdString()), Network::Regtest);
    require(first.is_ok && std::string(first.path) == "0/1", "the first receive address derives");
    chain->setProperty(
        "fixtureOutputs",
        QVariantList{QVariantMap{{QStringLiteral("outpoint"), CHAIN_COIN},
                                 {QStringLiteral("amount"), CHAIN_COIN_SATS},
                                 {QStringLiteral("script"),
                                  segwitScript(QString::fromStdString(std::string(first.address)))},
                                 {QStringLiteral("keychain"), 0},
                                 {QStringLiteral("index"), 1},
                                 {QStringLiteral("height"), CHAIN_COIN_HEIGHT}}});
    chain->setProperty(
        "fixtureUsed",
        QVariantList{QVariantMap{{QStringLiteral("keychain"), 0}, {QStringLiteral("last"), 1}},
                     QVariantMap{{QStringLiteral("keychain"), 1},
                                 {QStringLiteral("last"), CHAIN_USED_CHANGE}}});
    chain->setProperty("fixtureTip", CHAIN_TIP);
    emit chain->coinsChanged();
}

auto payment(const QStringList &inputs) -> backend::TransactionTemplate {
    return {.outputs = {backend::Output{.address = EXTERNAL, .amount = 40000}},
            .inputs = inputs,
            .feeRate = 2.0};
}

// The leaf of `key` alone, spending CHAIN_COIN.
auto hotLeaf(MusigAccountController *account, const ChainHotKey &key) -> MusigSpendLeaf {
    auto leaves = account->spendLeaves({CHAIN_COIN});
    require(leaves.is_ok, "the backend's coin has spend paths");
    auto leaf = std::find_if(leaves.leaves.begin(), leaves.leaves.end(), [&key](const auto &leaf) {
        return leaf.fingerprints.size() == 1 &&
               QString::fromStdString(std::string(leaf.fingerprints[0])) == key.fingerprint;
    });
    require(leaf != leaves.leaves.end() && leaf->spendable,
            "the hot key's leaf spends the backend's coin");
    return *leaf;
}

// Exports a spend of `crafted` for each change index the backend watches past the one it
// reports used, CHAIN_USED_CHANGE: none needs a reveal, and the next change is past the
// backend's window.
void exportThroughWindow(MusigAccountController *account, backend::ITransaction *transaction,
                         backend::IBlockchain *chain, const backend::TransactionTemplate &crafted,
                         const MusigSpendLeaf &leaf) {
    ExportReplies replies;
    QObject::connect(account, &MusigAccountController::transactionExported, &replies,
                     &ExportReplies::onExported, qontrol::UNIQUE);
    QObject::connect(account, &MusigAccountController::exportFailed, &replies,
                     &ExportReplies::onExportFailed, qontrol::UNIQUE);
    auto window = static_cast<int>(backend::AccountWatch{}.gapLimit);
    for (int count = 1; count <= window; ++count) {
        auto psbt = transaction->finalize(crafted);
        require(psbt.has_value(), "the spend finalizes on the next change index");
        account->exportTransaction(crafted, leaf, *psbt);
        waitFor(
            [&replies, count] { return replies.exported == count || !replies.failure.isEmpty(); },
            "the export answers");
        require(replies.failure.isEmpty(), "the spend is exported");
    }
    require(!chain->property("revealed").isValid(),
            "no change index the backend watches is revealed");
}

// Adopts `psbt`, ITransaction::finalize() of `crafted`, on the leaf of `key` alone, signs it
// with that hot key and imports the answer: the account finalizes the spend of CHAIN_COIN.
// Its change, `change_index`, is past what the backend watches: it is revealed first.
void spendThroughHotLeaf(MusigAccountController *account, backend::IBlockchain *chain,
                         const ChainHotKey &key, const backend::TransactionTemplate &crafted,
                         const QByteArray &psbt, quint32 change_index) {
    SignModalDriver driver(account, key);
    account->adoptTransaction(crafted, hotLeaf(account, key), psbt);
    waitFor([&driver] { return !driver.finalized.isEmpty(); }, "the adopted spend is signed");
    require(driver.finalized.contains(CHAIN_COIN.section(QLatin1Char(':'), 0, 0)),
            "the finalized transaction spends the backend's coin");
    require(chain->property("revealed").toString() == QStringLiteral("0/1/%1").arg(change_index),
            "the change past the backend's window is revealed before the spend is handed out");
    chain->setProperty("revealed", QVariant());
}

// Hands out receive addresses until the one at FIRST_UNWATCHED: the backend reveals that
// one alone, before it is handed out.
void receiveThroughReveal(backend::IAccountBackend *account, backend::IBlockchain *chain,
                          ChainReplies &replies) {
    for (quint32 index = 2; index <= FIRST_UNWATCHED; ++index) {
        require(!chain->property("revealed").isValid(), "no index the backend watches is revealed");
        auto count = replies.addresses.size();
        account->newAddress(account::nextRequestId(), QStringLiteral("taproot"));
        waitFor([&replies,
                 count] { return replies.addresses.size() > count || !replies.failure.isEmpty(); },
                "the receive address is handed out");
        require(replies.failure.isEmpty(), "the receive address is handed out without error");
    }
    require(chain->property("revealed").toString() == QStringLiteral("0/0/%1").arg(FIRST_UNWATCHED),
            "the first index past the backend's window is revealed before it is handed out");
    auto verification = account->addressVerification(replies.addresses.last().address);
    require(verification.has_value() &&
                verification->path == QStringLiteral("0/%1").arg(FIRST_UNWATCHED),
            "the revealed index is the address handed out");
}

} // namespace

// A Multikey account on the chain fixture, an Electrum server saved: it opens no Electrum
// connection, its status bar shows the backend, and it crafts, adopts, signs with a hot key
// and finalizes a spend from the coin the backend reports, hands out receive addresses past
// what the backend used, revealing the one past its window first, and keeps its labels in
// Multikey.
void chainSpendSmoke() {
    auto key = chainHotKey();
    auto *controller = AppController::get();
    auto multikey = std::make_unique<MusigPlugin>();
    auto *multikeyModule = qobject_cast<account::AccountModule *>(multikey->modules().first());
    controller->registerBuiltinPlugin(std::move(multikey));
    auto *registry = controller->pluginRegistry();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin state loads");
    enableFixture(registry, CHAIN, SILENT_SERVICE_CHAIN_FIXTURE);
    auto *chainModule = fixtureModule(registry, CHAIN);
    // Nothing listens there: a probe would fail, never connect.
    auto config = ::new_config(rust::String(MULTIKEY.toStdString()), Network::Regtest,
                               rust::String(key.descriptor.toStdString()), rust::String(""),
                               rust::String("127.0.0.1:1"), 0, rust::String("musig"));
    require(config->save().empty(), "the Multikey account saves");
    require(::save_blockchain_backend(rust::String(MULTIKEY.toStdString()),
                                      rust::String(CHAIN_MODULE.toStdString())),
            "the account chooses the chain plugin as its blockchain backend");
    MainWindow window;
    controller->start(&window);
    multikeyModule->startInstance(MULTIKEY);
    auto *widget = window.findChild<MusigAccountWidget *>();
    require(widget != nullptr, "the Multikey account opens in a tab");
    waitFor(
        [widget, chainModule] {
            return assembledBlockchain(widget) != nullptr &&
                   assembledBlockchain(widget)->parent() == chainModule &&
                   widget->shell()->fundsOpen();
        },
        "the account opens over the chain plugin's blockchain");
    auto *account = widget->controller();
    auto services = widget->shell()->services();
    // A probe of that server would fail within this wait and report it.
    spin(PROBE_MS);
    require(account->electrumState() == MusigAccountController::ElectrumState::Disconnected &&
                !account->endpointOperationsBusy() &&
                account->endpointError(Endpoint::Electrum).isEmpty(),
            "an Electrum server saved, the account probes and connects none");
    require(statusLabel(widget) == TR("status-backend").arg(CHAIN),
            "the status bar shows the chain plugin, not Electrum");
    account->setEndpointConnected(account::nextRequestId(), Endpoint::Electrum, true);
    require(account->electrumState() == MusigAccountController::ElectrumState::Disconnected &&
                !account->endpointOperationsBusy(),
            "a request to connect Electrum is refused");
    require(services.labels != nullptr && services.labels->parent() != chainModule,
            "the labels stay Multikey's");

    reportChainCoin(services.blockchain, key.descriptor);
    ChainReplies replies;
    QObject::connect(services.transaction, &backend::ITransaction::coinsSelected, &replies,
                     &ChainReplies::onCoinsSelected, qontrol::UNIQUE);
    services.transaction->selectCoins(account::nextRequestId(), payment({}));
    waitFor([&replies] { return replies.selected.has_value(); }, "coin selection answers");
    require(replies.selected->completion.complete &&
                replies.selected->transaction.inputs == QStringList{CHAIN_COIN},
            "coin selection picks the coin the backend reports");
    auto crafted = services.transaction->craft(replies.selected->transaction);
    require(crafted.completion.complete && crafted.transaction.fee > 0,
            "the backend's coin crafts the spend");
    auto psbt = services.transaction->finalize(crafted.transaction);
    require(psbt.has_value(), "the spend finalizes");
    auto signers = ::psbt_required_fingerprints(rust::Str(psbt->toHex().toStdString()));
    require(signers.is_ok && signers.fingerprints.size() == 3,
            "the input carries the derivation of the policy's three keys");
    require(psbt->contains(segwitScript(QString::fromStdString(
                std::string(::musig_first_receive_address(
                                rust::String(key.descriptor.toStdString()), Network::Regtest)
                                .address)))),
            "the input's witness UTXO is the backend's script");
    exportThroughWindow(account, services.transaction, services.blockchain, crafted.transaction,
                        hotLeaf(account, key));
    auto outside = services.transaction->finalize(crafted.transaction);
    require(outside.has_value(), "the spend finalizes past the backend's change window");
    spendThroughHotLeaf(account, services.blockchain, key, crafted.transaction, *outside,
                        CHAIN_USED_CHANGE + backend::AccountWatch{}.gapLimit + 1);

    QObject::connect(services.account, &backend::IAccountBackend::addressCreated, &replies,
                     &ChainReplies::onAddressCreated, qontrol::UNIQUE);
    QObject::connect(services.account, &backend::IService::error, &replies, &ChainReplies::onError,
                     qontrol::UNIQUE);
    receiveThroughReveal(services.account, services.blockchain, replies);

    chooseBackend(widget, QString());
    auto electrum = widget->shell()->services().transaction->craft(payment({CHAIN_COIN}));
    require(!electrum.completion.complete,
            "Electrum chosen again, the backend's coin is no longer the account's");

    Host::get()->removeInstance(MULTIKEY, multikeyModule);
    waitFor(
        [multikeyModule] {
            deleteDeferred();
            return !multikeyModule->accountUnavailable(MULTIKEY);
        },
        "the Multikey account closes");
    require(::delete_musig_account(rust::String(MULTIKEY.toStdString())).empty(),
            "the Multikey account is deleted");
}

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
    shellOneProvider(full);
    shellRetirement(registry, full);
}

// A Multikey account choosing the chain fixture as its blockchain backend: missing until the
// plugin is installed, then its blockchain watching the account; Electrum again hands it
// back, and the plugin disabled shows the missing backend again.
void blockchainBackendSmoke() {
    auto *controller = AppController::get();
    auto multikey = std::make_unique<MusigPlugin>();
    auto *multikeyModule = qobject_cast<account::AccountModule *>(multikey->modules().first());
    controller->registerBuiltinPlugin(std::move(multikey));
    auto *registry = controller->pluginRegistry();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin state loads");
    // Supplies a blockchain, but does not advertise that it watches descriptors.
    enableFixture(registry, FULL, SILENT_SERVICE_FULL_FIXTURE);
    auto *full = fixtureModule(registry, FULL);
    auto config = ::new_config(rust::String(MULTIKEY.toStdString()), Network::Regtest,
                               rust::String(MULTIKEY_DESCRIPTOR), rust::String(""),
                               rust::String(""), 0, rust::String("musig"));
    require(config->save().empty(), "the Multikey account saves");
    require(::save_blockchain_backend(rust::String(MULTIKEY.toStdString()),
                                      rust::String(CHAIN_MODULE.toStdString())),
            "the account chooses the chain plugin as its blockchain backend");
    MainWindow window;
    controller->start(&window);
    multikeyModule->startInstance(MULTIKEY);
    auto *widget = window.findChild<MusigAccountWidget *>();
    require(widget != nullptr, "the Multikey account opens in a tab");
    waitFor([widget] { return assembledBlockchain(widget) != nullptr; },
            "the account's services are assembled");
    requireMissingBackend(widget, "a chosen backend not installed opens Settings naming it, with "
                                  "no blockchain and no funds: no fallback to Electrum");
    require(outstanding(full) == 0, "a plugin not advertising descriptor watching is not used");

    enableFixture(registry, CHAIN, SILENT_SERVICE_CHAIN_FIXTURE);
    auto *chain = fixtureModule(registry, CHAIN);
    waitFor([widget, chain] { return assembledBlockchain(widget)->parent() == chain; },
            "installing the chosen backend assembles the services over its blockchain");
    requireChainBackend(widget, chain, 1, "the installed backend is handed the account's watch");
    auto *extras = backendExtras(widget);
    require(backendChoices(extras) == QStringList({QString(), CHAIN_MODULE}) &&
                extras->m_backend_picker->itemText(0) == TR("musig-backend-electrum") &&
                extras->m_backend_picker->itemText(1) == CHAIN &&
                extras->m_backend_picker->currentIndex() == 1,
            "Settings offers Electrum and the plugin advertising descriptor watching, by its "
            "name, the chosen one selected");
    waitFor([widget] { return widget->shell()->fundsOpen(); },
            "the funds panels open over the backend");
    rescanFromHeight(widget);

    chooseBackend(widget, QString());
    require(assembledBlockchain(widget)->parent() != chain && !unsupportedBlockchain(widget),
            "Electrum chosen, the blockchain is Multikey's own");
    require(backendExtras(widget)->m_rescan->isHidden(), "Electrum chosen, no rescan is offered");
    deleteDeferred();
    require(outstanding(chain) == 0 && savedBackend().isEmpty(),
            "Electrum chosen and saved, the chain plugin's blockchain is handed back");

    chooseBackend(widget, CHAIN_MODULE);
    requireChainBackend(widget, chain, 2,
                        "the plugin chosen again is handed the account's watch again");
    require(savedBackend() == CHAIN_MODULE, "the plugin choice is saved");
    createdAccountBirthday(window, multikeyModule, chain, widget, 3);
    restoredAccountBirthday(window, multikeyModule, chain, widget, 4);

    require(registry->setEnabled(CHAIN, false), "the chain plugin disable is admitted");
    waitFor(
        [registry, widget] {
            deleteDeferred();
            return !registry->isPluginEnabled(CHAIN) && unsupportedBlockchain(widget);
        },
        "the chain plugin stops");
    requireMissingBackend(widget, "the chosen backend disabled, Settings names it again");

    Host::get()->removeInstance(MULTIKEY, multikeyModule);
    waitFor(
        [multikeyModule] {
            deleteDeferred();
            return !multikeyModule->accountUnavailable(MULTIKEY);
        },
        "the Multikey account closes");
    require(::delete_musig_account(rust::String(MULTIKEY.toStdString())).empty(),
            "the Multikey account is deleted");
}

} // namespace smoke

#include "account_services_smoke.moc"
