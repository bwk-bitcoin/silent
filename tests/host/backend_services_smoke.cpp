// The built-in SP and Multikey providers: their services assembled for real Regtest
// accounts, and the published read model a polling and a notified consumer share, with
// a history that follows its coins later.

#include "AppController.h"
#include "MainWindow.h"
#include "account/AccountModule.h"
#include "account/services/ServiceAdapters.h"
#include "account/services/ServiceContext.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "host/ServiceAssembly.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/sp/AccountController.h"
#include "modules/sp/AccountWidget.h"
#include "modules/sp/SpPlugin.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QThread>
#include <common.h>
#include <functional>
#include <memory>
#include <optional>
#include <silent.h>

namespace smoke {
namespace {

constexpr int TIMEOUT_MS = 20000;
// Longer than the read model's one second reconcile interval.
constexpr int RECONCILE_WAIT_MS = 3000;

const QString SP_ACCOUNT = QStringLiteral("services-sp");
const QString MULTIKEY_ACCOUNT = QStringLiteral("services-multikey");
const char *const SP_DESCRIPTOR =
    "sp([73c5da0a/352'/1'/0']tspscan1q8pjcdy7qzlzxl44chw2tsanvzg7dtwhkqf3nsvzmdavls2ekl8qq9qe"
    "sshy6w9knddr825kqp442302zuwddh6vtqk7zqvgszace9aczgnuqn3)#ke33wu3h";
const char *const MULTIKEY_DESCRIPTOR =
    "tr(musig([4ba43603/87'/1'/0'/2]tpubDFGmrfdE8Bya4CPLde4Q54gxkY4KKJzzs6y5tGWxVEcrfmzZb17aCamv"
    "X9k1UwKPPHtd7u5WqyS5SiRwxBZvHhbCJumqwECQQycuAmYtfzZ,[8dfc9b34/87'/1'/0'/2]tpubDFWeBGy8cbQXd"
    "CA37ePK189L12ML8fZCuuG9EZtwybxBx2jrVRLrWwsXvRHvXuCS5Q6sRyLB92TsidhBVbwWoeTAxDsDC2w5vQmPVnL2v"
    "KB)/<0;1>/*,pk([56c4fac3/87'/1'/0'/2]tpubDE1s5mmJDMZQ9uwi2KHe99ioqqEpwwZbAmAWP1UK6Y6zHLTKQw"
    "hkGLeSsQi38VaBwzfWtUrSL7v4tD1DPmHKjSfXBRbXVYUVboM7idebk7T/<2;3>/*))";
const QString FUNDING = QString(64, QLatin1Char('a'));

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("backend services: %s", message);
    }
}

void spin(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
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

// A context whose funds read is scripted: a store that publishes a coin before the
// transaction funding it. Every request but transaction details answers at once.
class ScriptedContext final : public account::ServiceContext {
    Q_OBJECT

public:
    Funds funds;
    int reads = 0;

    ScriptedContext() : ServiceContext(QStringLiteral("scripted"), nullptr) {
    }

    auto craft(const backend::TransactionTemplate &transaction) -> backend::CraftResult override {
        return {.transaction = transaction};
    }

    auto finalize([[maybe_unused]] const backend::TransactionTemplate &transaction)
        -> std::optional<QByteArray> override {
        return std::nullopt;
    }

    void selectCoins(backend::RequestId token,
                     [[maybe_unused]] backend::TransactionTemplate transaction) override {
        emit failed(token, QString());
    }

    void newAddress(backend::RequestId token, [[maybe_unused]] QString kind) override {
        emit failed(token, QString());
    }

    void setLabel(backend::RequestId token, [[maybe_unused]] backend::LabelKey key,
                  [[maybe_unused]] QString text) override {
        emit failed(token, QString());
    }

    // Left pending: the context goes before it answers.
    void fetchTransactionDetails([[maybe_unused]] backend::RequestId token,
                                 [[maybe_unused]] QString txid) override {
    }

    void connectAll(backend::RequestId token, [[maybe_unused]] bool start) override {
        emit failed(token, QString());
    }

    void setConnected(backend::RequestId token, [[maybe_unused]] QString connection,
                      [[maybe_unused]] bool connected) override {
        emit failed(token, QString());
    }

    void refreshTip(backend::RequestId token, [[maybe_unused]] QString connection) override {
        emit failed(token, QString());
    }

    void testEndpoint(backend::RequestId token,
                      [[maybe_unused]] backend::Endpoint endpoint) override {
        emit failed(token, QString());
    }

    void saveSettings(backend::RequestId token,
                      [[maybe_unused]] backend::Settings settings) override {
        emit failed(token, QString());
    }

    void endpointCapabilities(backend::RequestId token,
                              [[maybe_unused]] QString endpoint) override {
        emit failed(token, QString());
    }

    void broadcast(backend::RequestId token, [[maybe_unused]] QByteArray transaction) override {
        emit failed(token, QString());
    }

protected:
    // Answers during the call.
    auto startFundsRead() -> bool override {
        ++reads;
        fundsRead(funds);
        return true;
    }

    auto readLabels() -> std::optional<QList<backend::Label>> override {
        return QList<backend::Label>{};
    }

    auto readReceiving() -> std::optional<Receiving> override {
        return Receiving{};
    }
};

// Whether the funds panels of an account exist before its first funds read answered, as
// seen once its load finished, before the read's answer comes back.
class PanelOrder final : public QObject {
    Q_OBJECT

public:
    QPointer<AccountBackend> controller;
    std::function<bool()> panels;
    bool checked = false;
    bool early = false;

public slots:

    void onLoaded() {
        auto *context = controller == nullptr ? nullptr : controller->services();
        if (context == nullptr) {
            return;
        }
        checked = true;
        early = early || (panels() && !context->fundsKnown());
    }
};

class Answers final : public QObject {
    Q_OBJECT

public:
    int coinsChanged = 0;
    int historyChanged = 0;
    int labelsChanged = 0;
    QList<backend::RequestId> errors;
    QString lastError;
    std::optional<backend::Address> address;
    std::optional<backend::CraftResult> selected;
    std::optional<backend::BroadcastResult> broadcast;
    QList<backend::RequestId> done;

public slots:

    void onCoinsChanged() {
        ++coinsChanged;
    }

    void onHistoryChanged() {
        ++historyChanged;
    }

    void onLabelsChanged() {
        ++labelsChanged;
    }

    void onError(backend::RequestId request_id, const QString &message) {
        errors.append(request_id);
        lastError = message;
    }

    void onAddressCreated([[maybe_unused]] backend::RequestId request_id,
                          const backend::Address &created) {
        address = created;
    }

    void onCoinsSelected([[maybe_unused]] backend::RequestId request_id,
                         const backend::CraftResult &result) {
        selected = result;
    }

    void onBroadcastFinished([[maybe_unused]] backend::RequestId request_id,
                             const backend::BroadcastResult &result) {
        broadcast = result;
    }

    void onDone(backend::RequestId request_id) {
        done.append(request_id);
    }
};

// An early coin is not a commit point: the history written after it reaches the same
// getters, for a consumer that polls and for one that listens, with no notification.
void delayedHistory() {
    auto *context = new ScriptedContext();
    std::unique_ptr<backend::IService> service(
        account::serviceAdapter(backend::ServiceKind::Blockchain, context, nullptr));
    auto *blockchain = qobject_cast<backend::IBlockchain *>(service.get());
    require(blockchain != nullptr, "the adapter is an IBlockchain");
    Answers answers;
    QObject::connect(blockchain, &backend::IBlockchain::coinsChanged, &answers,
                     &Answers::onCoinsChanged, qontrol::UNIQUE);
    QObject::connect(blockchain, &backend::IBlockchain::historyChanged, &answers,
                     &Answers::onHistoryChanged, qontrol::UNIQUE);
    QObject::connect(blockchain, &backend::IService::error, &answers, &Answers::onError,
                     qontrol::UNIQUE);

    context->funds = {.coins = {backend::Coin{.outpoint = FUNDING + QStringLiteral(":0"),
                                              .amount = 1000,
                                              .spendable = true}},
                      .historyComplete = false};
    context->onFundsChanged();
    require(blockchain->coins().size() == 1 && blockchain->history().isEmpty() &&
                answers.coinsChanged == 1 && answers.historyChanged == 0,
            "the early coin is published, its history not yet");

    // The store writes the transaction; nothing notifies it.
    context->funds.history = {backend::Transaction{.txid = FUNDING}};
    context->funds.historyComplete = true;
    spin(RECONCILE_WAIT_MS);
    require(blockchain->history().size() == 1 && blockchain->history().first().txid == FUNDING,
            "a polling consumer reads the later history");
    require(answers.historyChanged == 1 && answers.coinsChanged == 1,
            "a notified consumer is told once, and only of the history");
    auto reads = context->reads;
    spin(RECONCILE_WAIT_MS);
    require(context->reads == reads, "a complete history is not read again");

    blockchain->fetchTransactionDetails(7, FUNDING);
    delete context;
    require(answers.errors == QList<backend::RequestId>{7},
            "a request pending when the account goes answers error()");
    require(blockchain->coins().isEmpty() && blockchain->history().isEmpty(),
            "a closed account's getters answer empty data");
    blockchain->rescan(8, std::nullopt);
    require(answers.errors.last() == 8, "every request of a closed account answers error()");
}

void saveFixture(const QString &name, const char *descriptor, const char *plugin_id) {
    auto config = ::new_config(rust::String(name.toStdString()), Network::Regtest,
                               rust::String(descriptor), rust::String("http://127.0.0.1:1"),
                               rust::String(""), 0, rust::String(plugin_id));
    auto error = config->save();
    require(error.empty(), "the Regtest fixture account saves");
}

auto assemble(HostImpl *host, IModule *module, const QString &account, backend::Network network)
    -> host::AssemblyResult {
    return host->assembleServices({.account = account,
                                   .network = network,
                                   .account_backend = module,
                                   .blockchain = module,
                                   .transaction = module,
                                   .labels = module,
                                   .settings = module});
}

const backend::TransactionTemplate EMPTY{
    .outputs = {{.address = QStringLiteral("bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"),
                 .amount = 1000}},
    .fee = 200};

// The provider answers once the account has loaded. Requests then answer error() while
// the controller hands the account to its startup connection: crafting tells when the
// account is back.
auto assembleIdle(HostImpl *host, IModule *module, const QString &account)
    -> std::unique_ptr<host::ServiceAssembly> {
    host::AssemblyResult result;
    waitFor(
        [&] {
            result = assemble(host, module, account, backend::Network::Regtest);
            return result.assembly != nullptr;
        },
        "the open account's services assemble");
    auto *transaction = result.assembly->transaction();
    waitFor(
        [transaction] {
            return transaction->craft(EMPTY).completion.error.contains(QStringLiteral("No inputs"));
        },
        "the account is back from its startup connection");
    return std::move(result.assembly);
}

void connectAnswers(const host::ServiceAssembly &assembly, Answers &answers) {
    for (auto *service : std::initializer_list<backend::IService *>{
             assembly.accountBackend(), assembly.blockchain(), assembly.transaction(),
             assembly.labels(), assembly.settings()}) {
        QObject::connect(service, &backend::IService::error, &answers, &Answers::onError,
                         qontrol::UNIQUE);
    }
    QObject::connect(assembly.accountBackend(), &backend::IAccountBackend::addressCreated, &answers,
                     &Answers::onAddressCreated, qontrol::UNIQUE);
    QObject::connect(assembly.transaction(), &backend::ITransaction::coinsSelected, &answers,
                     &Answers::onCoinsSelected, qontrol::UNIQUE);
    QObject::connect(assembly.blockchain(), &backend::IBlockchain::broadcastFinished, &answers,
                     &Answers::onBroadcastFinished, qontrol::UNIQUE);
    QObject::connect(assembly.labels(), &backend::ILabels::labelsChanged, &answers,
                     &Answers::onLabelsChanged, qontrol::UNIQUE);
    QObject::connect(assembly.labels(), &backend::ILabels::saved, &answers, &Answers::onDone,
                     qontrol::UNIQUE);
    QObject::connect(assembly.settings(), &backend::IBackendSettings::saved, &answers,
                     &Answers::onDone, qontrol::UNIQUE);
    QObject::connect(assembly.blockchain(), &backend::IBlockchain::completed, &answers,
                     &Answers::onDone, qontrol::UNIQUE);
}

// A settings write waits for the endpoint operation reading and writing the same config.
void settingsWaitForEndpoints(const host::ServiceAssembly &assembly, Answers &answers) {
    auto settings = assembly.settings()->settings();
    settings.endpoints.first().url = QStringLiteral("tcp://127.0.0.1:1");
    assembly.blockchain()->setConnected(20, QStringLiteral("electrum"), false);
    assembly.settings()->save(21, settings);
    require(answers.errors.contains(21) &&
                answers.lastError.contains(QStringLiteral("endpoint operation")),
            "a save during an endpoint operation is refused");
    waitFor([&answers] { return answers.done.contains(20) || answers.errors.contains(20); },
            "the disconnection answers");
    assembly.settings()->save(22, settings);
    // The running save is itself an endpoint operation: nothing else writes the config meanwhile.
    assembly.settings()->save(23, settings);
    require(answers.errors.contains(23) &&
                answers.lastError.contains(QStringLiteral("endpoint operation")),
            "a write during a running save is refused");
    waitFor([&answers] { return answers.done.contains(22) || answers.errors.contains(22); },
            "the save answers");
    require(answers.done.contains(22) && assembly.settings()->settings().endpoints.first().url ==
                                             QStringLiteral("tcp://127.0.0.1:1"),
            "the save once idle persists and publishes the endpoint");
}

// What both built-in providers answer alike.
void commonServices(const host::ServiceAssembly &assembly, Answers &answers,
                    const QStringList &connections) {
    require(assembly.accountBackend()->id() == assembly.account() &&
                assembly.accountBackend()->network() == backend::Network::Regtest,
            "the account service names its account and network");
    auto settings = assembly.settings()->settings();
    QStringList endpoints;
    for (const auto &endpoint : settings.endpoints) {
        endpoints.append(endpoint.id);
    }
    require(settings.network == backend::Network::Regtest && !settings.networkEditable &&
                endpoints == connections,
            "the settings list the account's endpoints");
    QStringList listed;
    for (const auto &connection : assembly.blockchain()->state().connections) {
        listed.append(connection.id);
    }
    require(listed == connections, "the blockchain state lists the same connections");
    require(assembly.blockchain()->coins().isEmpty() &&
                assembly.blockchain()->balance().confirmed == 0,
            "an empty wallet publishes no coin");

    auto empty = EMPTY;
    auto crafted = assembly.transaction()->craft(empty);
    require(!crafted.completion.complete && crafted.transaction.inputs.isEmpty() &&
                !assembly.transaction()->finalize(empty).has_value(),
            "crafting and finalizing select no coin");
    assembly.transaction()->selectCoins(11, empty);
    waitFor([&answers] { return answers.selected.has_value(); }, "coin selection answers");
    require(!answers.selected->completion.complete &&
                answers.selected->transaction.inputs.isEmpty(),
            "selection over an empty wallet is incomplete");

    backend::LabelKey key{.kind = backend::LabelKind::Transaction, .id = FUNDING};
    assembly.labels()->setLabel(12, key, QStringLiteral("smoke label"));
    waitFor([&answers] { return answers.done.contains(12); }, "the label saves");
    require(assembly.labels()->label(key) == QStringLiteral("smoke label") &&
                answers.labelsChanged == 1,
            "the saved label is published before saved()");
    assembly.labels()->setLabel(13, key, QString());
    waitFor([&answers] { return answers.done.contains(13); }, "the label removal saves");
    require(assembly.labels()->label(key).isEmpty() && assembly.labels()->labels().isEmpty(),
            "an empty label removes it");

    assembly.blockchain()->broadcast(14, QByteArrayLiteral("not a transaction"));
    waitFor([&answers] { return answers.broadcast.has_value(); }, "the broadcast answers");
    require(answers.broadcast->outcome == backend::BroadcastOutcome::Rejected &&
                !answers.broadcast->error.isEmpty(),
            "an undecodable transaction is rejected, never sent");

    assembly.blockchain()->rescan(15, std::nullopt);
    assembly.accountBackend()->backup(16, QByteArrayLiteral("password"));
    assembly.settings()->value(17, QStringLiteral("anything"), QJsonValue());
    require(answers.errors.size() >= 3 && answers.errors.sliced(answers.errors.size() - 3) ==
                                              QList<backend::RequestId>{15, 16, 17},
            "unsupported requests answer error()");
}

void spServices(HostImpl *host, IModule *module) {
    auto assembly = assembleIdle(host, module, SP_ACCOUNT);
    Answers answers;
    connectAnswers(*assembly, answers);
    auto kinds = assembly->accountBackend()->addressKinds();
    require(kinds.size() == 1 && kinds.first().id == QStringLiteral("sp") && kinds.first().reusable,
            "an SP account receives on its static address");
    auto addresses = assembly->accountBackend()->addresses();
    require(addresses.size() == 1 && addresses.first().kind == QStringLiteral("sp"),
            "the SP address is published");
    auto verify = assembly->accountBackend()->addressVerification(addresses.first().address);
    require(verify.has_value() && verify->path == QStringLiteral("m") &&
                verify->descriptor == QString::fromLatin1(SP_DESCRIPTOR),
            "the SP address verifies against the account descriptor");
    assembly->accountBackend()->newAddress(10, QStringLiteral("sp"));
    waitFor([&answers] { return answers.address.has_value(); }, "the SP address is handed out");
    require(answers.address->address == addresses.first().address,
            "the SP kind hands out its static address");
    commonServices(*assembly, answers, {QStringLiteral("blindbit"), QStringLiteral("electrum")});
}

void multikeyServices(HostImpl *host, IModule *module) {
    auto assembly = assembleIdle(host, module, MULTIKEY_ACCOUNT);
    auto refused = assemble(host, module, MULTIKEY_ACCOUNT, backend::Network::Testnet4);
    require(refused.assembly == nullptr && !refused.error.isEmpty(),
            "a loaded account is not supplied on another network, Testnet4 included");
    Answers answers;
    connectAnswers(*assembly, answers);
    auto kinds = assembly->accountBackend()->addressKinds();
    require(kinds.size() == 1 && kinds.first().id == QStringLiteral("taproot") &&
                !kinds.first().reusable,
            "a Multikey account receives on taproot addresses");
    require(assembly->accountBackend()->addresses().isEmpty(),
            "no address is handed out before the first request");
    assembly->accountBackend()->newAddress(10, QStringLiteral("taproot"));
    waitFor([&answers] { return answers.address.has_value(); }, "a taproot address is created");
    auto addresses = assembly->accountBackend()->addresses();
    require(addresses.size() == 1 && addresses.first().address == answers.address->address,
            "the created address is published before addressCreated()");
    auto verify = assembly->accountBackend()->addressVerification(answers.address->address);
    require(verify.has_value() && verify->path == QStringLiteral("0/1") &&
                !verify->descriptor.isEmpty(),
            "the created address verifies on its derivation path");
    commonServices(*assembly, answers, {QStringLiteral("electrum")});
    settingsWaitForEndpoints(*assembly, answers);
}

void closeAccount(account::AccountModule *module, const QString &id) {
    Host::get()->removeInstance(id, module);
    waitFor([module, &id] { return !module->accountUnavailable(id); }, "the account closes");
}

auto electrumConnecting(const AccountBackend &controller) -> bool {
    return controller.endpointOperation(Endpoint::Electrum) == EndpointOperation::Connecting;
}

// Selects coins of the account while `controller` tries to connect Electrum: `inputs` are
// selected, and the attempt still runs once the selection answered.
void selectWhileConnecting(const host::ServiceAssembly &assembly, Answers &answers,
                           const AccountBackend &controller, backend::RequestId token,
                           const QStringList &inputs) {
    require(electrumConnecting(controller), "a connection attempt runs");
    answers.selected.reset();
    assembly.transaction()->selectCoins(token, EMPTY);
    waitFor([&answers,
             token] { return answers.selected.has_value() || answers.errors.contains(token); },
            "coin selection answers");
    require(!answers.errors.contains(token), "coin selection is not refused during the attempt");
    require(answers.selected->completion.complete == !inputs.isEmpty() &&
                answers.selected->transaction.inputs == inputs,
            "coin selection answers on the account's coins during the attempt");
    require(electrumConnecting(controller), "the attempt still runs once the selection answered");
}

// Waits for the attempt on the silent server to fail, Electrum still disconnected.
void waitForFailedAttempt(const AccountBackend &controller) {
    waitFor(
        [&controller] {
            return controller.endpointOperation(Endpoint::Electrum) == EndpointOperation::Idle &&
                   !controller.endpointError(Endpoint::Electrum).isEmpty();
        },
        "the connection attempt fails");
    require(!controller.isEndpointRunning(Endpoint::Electrum),
            "the failed attempt leaves Electrum disconnected");
}

auto assembleOpen(HostImpl *host, IModule *module, const QString &account)
    -> std::unique_ptr<host::ServiceAssembly> {
    host::AssemblyResult result;
    waitFor(
        [&] {
            result = assemble(host, module, account, backend::Network::Regtest);
            return result.assembly != nullptr;
        },
        "the open account's services assemble");
    return std::move(result.assembly);
}

} // namespace

void backendServicesSmoke() {
    delayedHistory();

    saveFixture(SP_ACCOUNT, SP_DESCRIPTOR, "sp");
    saveFixture(MULTIKEY_ACCOUNT, MULTIKEY_DESCRIPTOR, "musig");
    auto sp = std::make_unique<SpPlugin>();
    auto multikey = std::make_unique<MusigPlugin>();
    auto *spModule = qobject_cast<account::AccountModule *>(sp->modules().first());
    auto *multikeyModule = qobject_cast<account::AccountModule *>(multikey->modules().first());
    require(spModule != nullptr && multikeyModule != nullptr, "both modules are account modules");
    require(qobject_cast<backend::IServiceProvider *>(spModule) != nullptr &&
                qobject_cast<backend::IServiceProvider *>(multikeyModule) != nullptr,
            "both built-in modules provide account services");
    auto *controller = AppController::get();
    controller->registerBuiltinPlugin(std::move(sp));
    controller->registerBuiltinPlugin(std::move(multikey));
    auto *registry = controller->pluginRegistry();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin state loads");
    MainWindow window;
    controller->start(&window);
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the host is the real host");

    require(assemble(host, spModule, SP_ACCOUNT, backend::Network::Regtest).assembly == nullptr,
            "a closed account supplies no service");
    spModule->startInstance(SP_ACCOUNT);
    multikeyModule->startInstance(MULTIKEY_ACCOUNT);
    auto *spWidget = window.findChild<AccountWidget *>();
    auto *multikeyWidget = window.findChild<MusigAccountWidget *>();
    require(spWidget != nullptr && multikeyWidget != nullptr, "both accounts open in tabs");
    PanelOrder spOrder;
    spOrder.controller = spWidget->controller();
    spOrder.panels = [controller = spWidget->controller()] {
        return controller->screen(QStringLiteral("history")).has_value();
    };
    PanelOrder multikeyOrder;
    multikeyOrder.controller = multikeyWidget->controller();
    multikeyOrder.panels = [controller = multikeyWidget->controller()] {
        return controller->screen(QStringLiteral("history")).has_value();
    };
    // Queued: checked right after the load, before the first funds read can answer.
    for (auto *order : {&spOrder, &multikeyOrder}) {
        QObject::connect(order->controller, &AccountBackend::accountLoadFinished, order,
                         &PanelOrder::onLoaded,
                         Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    }
    spServices(host, spModule);
    multikeyServices(host, multikeyModule);
    require(spOrder.checked && multikeyOrder.checked && !spOrder.early && !multikeyOrder.early,
            "the funds panels are built once the first funds read answered, not empty before");
    require(spOrder.panels() && multikeyOrder.panels(), "the funds panels are built after it");
    closeAccount(spModule, SP_ACCOUNT);
    closeAccount(multikeyModule, MULTIKEY_ACCOUNT);
    require(assemble(host, multikeyModule, MULTIKEY_ACCOUNT, backend::Network::Regtest).assembly ==
                nullptr,
            "a closed account supplies no new service");
}

// The funded Multikey `account`, whose Electrum server never answers, selects its coin
// `outpoint` while it connects at startup and while it reconnects. An SP account on the
// same server, which the harness cannot fund, answers its empty selection while it
// reconnects.
void offlineSelectionSmoke(const QString &account, const QString &outpoint) {
    auto electrum = ::config_from_file(rust::String(account.toStdString()))->get_electrum_url();
    auto spConfig = ::new_config(rust::String(SP_ACCOUNT.toStdString()), Network::Regtest,
                                 rust::String(SP_DESCRIPTOR), rust::String("http://127.0.0.1:1"),
                                 electrum, 0, rust::String("sp"));
    require(spConfig->save().empty(), "the SP fixture account saves");
    auto sp = std::make_unique<SpPlugin>();
    auto multikey = std::make_unique<MusigPlugin>();
    auto *spModule = qobject_cast<account::AccountModule *>(sp->modules().first());
    auto *multikeyModule = qobject_cast<account::AccountModule *>(multikey->modules().first());
    require(spModule != nullptr && multikeyModule != nullptr, "both modules are account modules");
    auto *controller = AppController::get();
    controller->registerBuiltinPlugin(std::move(sp));
    controller->registerBuiltinPlugin(std::move(multikey));
    auto *registry = controller->pluginRegistry();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin state loads");
    MainWindow window;
    controller->start(&window);
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the host is the real host");

    multikeyModule->startInstance(account);
    auto *multikeyWidget = window.findChild<MusigAccountWidget *>();
    require(multikeyWidget != nullptr, "the Multikey account opens in a tab");
    auto *multikeyController = multikeyWidget->controller();
    auto multikeyAssembly = assembleOpen(host, multikeyModule, account);
    Answers multikeyAnswers;
    connectAnswers(*multikeyAssembly, multikeyAnswers);
    selectWhileConnecting(*multikeyAssembly, multikeyAnswers, *multikeyController, 30, {outpoint});
    waitForFailedAttempt(*multikeyController);
    // A reconnect, as the retry timer starts it.
    multikeyController->onElectrumReconnect();
    selectWhileConnecting(*multikeyAssembly, multikeyAnswers, *multikeyController, 31, {outpoint});
    waitForFailedAttempt(*multikeyController);

    // With no sub-account, the SP account connects no Electrum at startup.
    spModule->startInstance(SP_ACCOUNT);
    auto *spWidget = window.findChild<AccountWidget *>();
    require(spWidget != nullptr, "the SP account opens in a tab");
    auto *spController = spWidget->controller();
    auto spAssembly = assembleIdle(host, spModule, SP_ACCOUNT);
    Answers spAnswers;
    connectAnswers(*spAssembly, spAnswers);
    waitFor([spController] { return !spController->endpointOperationsBusy(); },
            "the SP startup connection ends");
    spController->onElectrumReconnect();
    selectWhileConnecting(*spAssembly, spAnswers, *spController, 32, {});
    waitForFailedAttempt(*spController);

    multikeyAssembly.reset();
    spAssembly.reset();
    closeAccount(multikeyModule, account);
    closeAccount(spModule, SP_ACCOUNT);
}

} // namespace smoke

#include "backend_services_smoke.moc"
