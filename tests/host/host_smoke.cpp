// Opt-in plugin host/registry smoke test.
//
// Replaces the removed runtime env-var smoke (runDevTabSmokeIfRequested) with a
// headless lifecycle exercise driven by MOCK plugins. It never touches
// MainWindow or real tabs: it only drives Host::registerInstance/instances/
// removeInstance and the PluginRegistry builtin path.
//
// Built only when SILENT_BUILD_HOST_SMOKE=ON. Run under QT_QPA_PLATFORM=offscreen
// and a throwaway HOME so the Rust app-state FFI (~/.silent/app.json) stays
// hermetic. Exits 0 on success, non-zero on the first failed check.

#include <cstdio>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QThread>

#include <Qt>
#include <QtContainerFwd>
#include <QtCore/qtmetamacros.h>
#include <algorithm>
#include <common.h>
#include <memory>
#include <optional>
#include <silent.h>
#include <utility>

#include <AppController.h>
#include <MainWindow.h>
#include <host/Host.h>
#include <host/PluginRegistry.h>
#include <i18n/I18nManager.h>
#include <interfaces/enums.h>
#include <interfaces/host.h>
#include <interfaces/instance.h>
#include <interfaces/module.h>
#include <interfaces/signer.h>
#include <interfaces/types.h>
#include <modules/hwi/HwiSigner.h>
#include <modules/keys/DeviceName.h>
#include <modules/keys/KeysModule.h>
#include <modules/musig/MusigModule.h>
#include <modules/musig/MusigPlugin.h>
#include <modules/musig/creator/MusigCreatorModule.h>
#include <modules/sp/SpPlugin.h>
#include <theme/Theme.h>

#include "../gui/tx_flow_hover.h"
#include "ExternalSpendConsumer.h"
#include "MockPlugin.h"
#include "PriceProbe.h"
#include "StorageProbe.h"
#include "access_async_smoke.h"
#include "example_storage_smoke.h"
#include "fee_persistence_smoke.h"
#include "registry_async_errors.h"

namespace {

bool gOk = true;

// Shared by two devices holding the same seed, exposed by different modules.
const QString K_FINGERPRINT = QStringLiteral("a4f9c821");

void check(bool cond, const char *msg) {
    if (!cond) {
        gOk = false;
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
}

auto hostInstanceCount() -> int {
    return dynamic_cast<HostImpl *>(Host::get())->liveInstances().size();
}

auto hostInstance(const QString &id) -> IInstance * {
    for (auto *instance : dynamic_cast<HostImpl *>(Host::get())->liveInstances()) {
        if (instance != nullptr && instance->id() == id) {
            return instance;
        }
    }
    return nullptr;
}

auto hostHasInstance(const QString &id) -> bool {
    return hostInstance(id) != nullptr;
}

auto signerIds() -> QStringList {
    QStringList ids;
    for (const auto &entry : Host::get()->signers()) {
        ids.append(entry.instanceId);
    }
    return ids;
}

// Waits for the worker to return from its host call WITHOUT running the GUI
// thread's event loop. That separation is the point: once this returns, anything
// the host still owes has to be sitting in the GUI thread's queue.
auto waitForCalls(const smoke::ThreadedCaller &caller, int count, int timeout_ms) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (caller.callsDone() < count && timer.elapsed() < timeout_ms) {
        QThread::msleep(1);
    }
    return caller.callsDone() >= count;
}

// Spins the GUI thread until the instance reaches the expected state, so a
// regression fails on the check instead of hanging.
auto waitForInstance(const QString &id, bool present, int timeout_ms) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (hostHasInstance(id) != present && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return hostHasInstance(id) == present;
}

// Spins the GUI thread until the spy has seen the expected count of signals.
auto waitForSignals(const smoke::SignalSpy &spy, int count, int timeout_ms) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (spy.count() < count && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return spy.count() >= count;
}

// Keeps the last instances() answer of a module and counts its errors.
class ModuleSpy final : public QObject {
    Q_OBJECT

public:
    [[nodiscard]] auto instances() const -> QList<QPair<QString, QString>> {
        return m_instances;
    }

    [[nodiscard]] auto errors() const -> int {
        return m_errors;
    }

    [[nodiscard]] auto waitForInstances(ReqId req_id, int timeout_ms) const -> bool {
        QElapsedTimer timer;
        timer.start();
        while (m_request != req_id && timer.elapsed() < timeout_ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return m_request == req_id;
    }

public slots:

    void onInstances(ReqId req_id, const QList<QPair<QString, QString>> &instances) {
        m_request = req_id;
        m_instances = instances;
    }

    void onError([[maybe_unused]] std::optional<ReqId> req_id,
                 [[maybe_unused]] const QString &message) {
        ++m_errors;
    }

private:
    QList<QPair<QString, QString>> m_instances;
    ReqId m_request = 0;
    int m_errors = 0;
};

auto launcherLists(const PluginRegistry &registry, const IModule *module, const QString &id)
    -> bool {
    return std::ranges::any_of(registry.launcherInstancesForModule(module),
                               [&id](const auto &entry) { return entry.first == id; });
}

} // namespace

namespace smoke {
auto validatedInputGeometry() -> bool;
void pluginAbiSmoke();
auto pluginGnuHashChild(const QString &path, bool expected_admission) -> int;
void legacyPluginAbiSmoke(const QString &plugin_id);
void spSigningSmoke(const QString &name);
void musigResetSmoke();
void qrCacheExchangeSmoke();
void boundarySmoke();
void endpointSmoke();
void navigationSmoke();
void deletionSmoke();
void keysContractSmoke(const QString &screenshots);
void pluginBundleSmoke(const QString &bundle);
void pluginRemovalSmoke();
void pluginRemovalUiSmoke(const QString &shots);
void pluginRemovalReadFailureSmoke(const QString &shots);
void pluginAccessSettingsFailureSmoke(const QString &shots);
void accountServicesSmoke();
void backendServicesSmoke();
void offlineSelectionSmoke(const QString &account, const QString &outpoint);
void psbtTransferSmoke(const QString &account, const QString &psbt_file, const QString &txid,
                       const QString &outpoint);
void accountViewsSmoke();
void accountShellSmoke();
} // namespace smoke

auto main(int argc, char *argv[]) -> int {
    QApplication app(argc, argv);
    qRegisterMetaType<plugin::SignError>();
    qRegisterMetaType<std::optional<ReqId>>();
    i18n::I18nManager::get()->init(&app);
    auto arguments = QApplication::arguments();
#ifdef SILENT_ABI2_FIXTURE_PATH
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--plugin-gnu-hash-child")) {
        return smoke::pluginGnuHashChild(arguments.at(2),
                                         arguments.at(3) == QStringLiteral("admit"));
    }
#endif
    if (arguments.size() == 3 && (arguments.at(1) == QStringLiteral("--external-spend") ||
                                  arguments.at(1) == QStringLiteral("--external-nonce-review") ||
                                  arguments.at(1) == QStringLiteral("--external-lifecycle"))) {
        auto error = ::state_unlock_with_password(rust::String("consumer password"));
        check(error.empty(), "external consumer state unlocks");
        Host::get();
        AppController::init();
        Theme::init();
        auto sp = std::make_unique<SpPlugin>();
        auto multikey = std::make_unique<MusigPlugin>();
        auto *spModule = sp->modules().first();
        auto *multikeyModule = multikey->modules().first();
        AppController::get()->registerBuiltinPlugin(std::move(sp));
        AppController::get()->registerBuiltinPlugin(std::move(multikey));
        AppController::get()->registerBuiltinPlugin(std::make_unique<keys::KeysModule>());
        smoke::MockPlugin consumer;
        auto *registry = AppController::get()->pluginRegistry();
        registry->registerBuiltin(&consumer);
        MainWindow window;
        AppController::get()->start(&window);
        auto lifecycle = arguments.at(1) == QStringLiteral("--external-lifecycle");
        auto nonceReview =
            lifecycle || arguments.at(1) == QStringLiteral("--external-nonce-review");
        if (nonceReview) {
            multikeyModule->startInstance(QStringLiteral("external-musig-rounds"));
            multikeyModule->startInstance(QStringLiteral("external-musig-other"));
        } else {
            spModule->startInstance(QStringLiteral("external-sp"));
            multikeyModule->startInstance(QStringLiteral("external-multikey"));
        }
        smoke::ExternalSpendConsumer probe(Host::get(), consumer.module());
        probe.run(arguments.at(2));
        if (lifecycle) {
            auto *host = dynamic_cast<HostImpl *>(Host::get());
            check(host != nullptr, "host supports registry caller teardown");
            host->dropCaller(consumer.module());
            probe.waitForRetirement();
        }
        if (nonceReview) {
            Host::get()->removeInstance(QStringLiteral("external-musig-rounds"), multikeyModule);
            Host::get()->removeInstance(QStringLiteral("external-musig-other"), multikeyModule);
        } else {
            Host::get()->removeInstance(QStringLiteral("external-sp"), spModule);
            Host::get()->removeInstance(QStringLiteral("external-multikey"), multikeyModule);
        }
        std::fprintf(stdout, "external spend consumer: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--sp-signing")) {
        auto error = ::state_unlock_with_device(rust::String(arguments.at(2).toStdString()));
        check(error.empty(), "the funded SP fixture state should unlock");
        Host::get();
        AppController::init();
        Theme::init();
        smoke::spSigningSmoke(arguments.at(3));
        std::fprintf(stdout, "SP signing smoke: OK\n");
        return gOk ? 0 : 1;
    }

    // The app state is encrypted: a throwaway one, opened with a password.
    auto stateError = ::state_unlock_info().exists
                          ? ::state_unlock_with_password(rust::String("host smoke"))
                          : ::state_create_with_password(rust::String("host smoke"));
    check(stateError.empty(), "the app state should open");

    // Initialise the host singleton before the registry connects to its events.
    Host::get();
    AppController::init();
    if (arguments.contains(QStringLiteral("--fee-persistence"))) {
        smoke::feePersistenceSmoke();
        std::fprintf(stdout, "Fee persistence smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--tx-flow-hover"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::txFlowHoverSmoke();
        std::fprintf(stdout, "TxFlow hover smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--account-views"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::accountViewsSmoke();
        std::fprintf(stdout, "Account views smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--account-shell"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::accountShellSmoke();
        std::fprintf(stdout, "Account shell smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--offline-selection")) {
        Theme::init();
        Theme::get()->apply();
        smoke::offlineSelectionSmoke(arguments.at(2), arguments.at(3));
        std::fprintf(stdout, "Offline selection smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.size() == 6 && arguments.at(1) == QStringLiteral("--psbt-transfer")) {
        Theme::init();
        Theme::get()->apply();
        smoke::psbtTransferSmoke(arguments.at(2), arguments.at(3), arguments.at(4),
                                 arguments.at(5));
        std::fprintf(stdout, "PSBT transfer smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--backend-services"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::backendServicesSmoke();
        std::fprintf(stdout, "Backend services smoke: OK\n");
        return gOk ? 0 : 1;
    }
#ifdef SILENT_SERVICE_FULL_FIXTURE
    if (arguments.contains(QStringLiteral("--account-services"))) {
        // The account shell is themed.
        Theme::init();
        Theme::get()->apply();
        smoke::accountServicesSmoke();
        std::fprintf(stdout, "Account services smoke: OK\n");
        return gOk ? 0 : 1;
    }
#endif
#ifdef SILENT_REMOVAL_FIXTURE
    if (arguments.contains(QStringLiteral("--registry-async-errors"))) {
        Theme::init();
        Theme::get()->apply();
        AppController::get()->initState();
        smoke::registryCheckpointFailureSmoke();
        smoke::registryEnableRollbackFailureSmoke();
        smoke::registryEnableRollbackReadErrorSmoke();
        smoke::registryStaleScanErrorsSmoke();
        smoke::registryAccountDeletionRefreshSmoke();
        std::fprintf(stdout, "Registry async errors smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--access-async"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::accessAsyncSmoke();
        std::fprintf(stdout, "Access async smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--publication-heartbeat"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::publicationHeartbeatSmoke();
        std::fprintf(stdout, "Publication heartbeat smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--plugin-access-settings-failure"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::pluginAccessSettingsFailureSmoke(arguments.size() > 2 ? arguments.at(2) : QString());
        std::fprintf(stdout, "Plugin access Settings failure smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--plugin-removal-read-failure"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::pluginRemovalReadFailureSmoke(arguments.size() > 2 ? arguments.at(2) : QString());
        std::fprintf(stdout, "Plugin removal read failure smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--plugin-removal"))) {
        smoke::pluginRemovalSmoke();
        std::fprintf(stdout, "Plugin removal smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--plugin-removal-ui"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::pluginRemovalUiSmoke(arguments.size() > 2 ? arguments.at(2) : QString());
        std::fprintf(stdout, "Plugin removal UI smoke: OK\n");
        return gOk ? 0 : 1;
    }
#endif
    if (arguments.contains(QStringLiteral("--keys-contract"))) {
        Theme::init();
        smoke::keysContractSmoke(arguments.size() > 2 ? arguments.at(2) : QString());
        std::fprintf(stdout, "Keys contract smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--storage-example")) {
        Theme::init();
        AppController::get()->registerBuiltinPlugin(std::make_unique<smoke::MockPlugin>());
        smoke::exampleStorageSmoke(arguments.at(2));
        std::fprintf(stdout, "Example storage smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--storage-example-reinstall")) {
        Theme::init();
        AppController::get()->registerBuiltinPlugin(std::make_unique<smoke::MockPlugin>());
        smoke::exampleReinstallSmoke(arguments.at(2));
        std::fprintf(stdout, "Example reinstall smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--plugin-bundle")) {
        Theme::init();
        smoke::pluginBundleSmoke(arguments.at(2));
        std::fprintf(stdout, "Plugin bundle smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--account-navigation"))) {
        Theme::init();
        smoke::navigationSmoke();
        std::fprintf(stdout, "Account navigation smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--endpoints"))) {
        Theme::init();
        smoke::endpointSmoke();
        std::fprintf(stdout, "Endpoint smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--account-deletion"))) {
        Theme::init();
        Theme::get()->apply();
        smoke::deletionSmoke();
        std::fprintf(stdout, "Account deletion smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--rust-boundary"))) {
        Theme::init();
        smoke::boundarySmoke();
        std::fprintf(stdout, "Rust boundary smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--musig-reset"))) {
        Theme::init();
        smoke::musigResetSmoke();
        std::fprintf(stdout, "MuSig reset smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--qr-cache-exchange"))) {
        Theme::init();
        smoke::qrCacheExchangeSmoke();
        std::fprintf(stdout, "QR cache exchange smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--storage-contract"))) {
        Theme::init();
        check(smoke::storageSmoke(QStringLiteral("host smoke")), "storage contract");
        std::fprintf(stdout, "storage contract smoke: OK\n");
        return gOk ? 0 : 1;
    }
#ifdef SILENT_ABI2_FIXTURE_PATH
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--legacy-plugin-abi")) {
        smoke::legacyPluginAbiSmoke(arguments.at(2));
        std::fprintf(stdout, "legacy plugin ABI smoke: OK\n");
        return gOk ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--plugin-abi"))) {
        smoke::pluginAbiSmoke();
        std::fprintf(stdout, "plugin ABI smoke: OK\n");
        return gOk ? 0 : 1;
    }
    smoke::pluginAbiSmoke();
#endif
    check(smoke::priceFeedSmoke(), "price feed lifecycle and Send invariants");
    check(smoke::validatedInputGeometry(), "validated input geometry");

    smoke::MockPlugin plugin;
    check(qobject_cast<IPlugin *>(&plugin) == &plugin &&
              plugin.qt_metacast("dev.silent.IPlugin/2.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/3.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/4.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/5.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/6.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/7.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/8.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/9.0") == nullptr &&
              plugin.qt_metacast("dev.silent.IPlugin/11.0") == nullptr,
          "mock plugin metadata exposes ABI 10 only");
    auto *module = plugin.module();

    PluginRegistry registry;
    smoke::SignalSpy spy;
    QObject::connect(&registry, &PluginRegistry::enabledPluginsChanged, &spy,
                     &smoke::SignalSpy::onSignal, qontrol::UNIQUE);

    int baselineInstances = hostInstanceCount();

    // ===== registerBuiltin: enabled + builtin =====
    registry.registerBuiltin(&plugin);
    check(registry.isPluginEnabled(QStringLiteral("mock.plugin")),
          "mock plugin should be enabled after registerBuiltin");
    check(registry.isPluginBuiltin(QStringLiteral("mock.plugin")),
          "mock plugin should be marked builtin");

    // ===== enabledModulesForLauncher contains the module =====
    check(registry.enabledModulesForLauncher().contains(module),
          "enabledModulesForLauncher should contain the mock module");

    // setEnabled(false) on a builtin is a no-op by design (guarded by
    // pluginHasBuiltinModules); it must stay enabled.
    registry.setEnabled(QStringLiteral("mock.plugin"), false);
    check(registry.isPluginEnabled(QStringLiteral("mock.plugin")),
          "builtin plugin must remain enabled (setEnabled false is a no-op)");

    // Deliver the module's pending list()/autoStart() responses now that the
    // registry has recorded the pending requests during registration.
    module->flush();

    // ===== launcher instances populated via the list() response =====
    auto launcherInstances = registry.launcherInstancesForModule(module);
    bool hasInst1 = false;
    for (const auto &entry : launcherInstances) {
        if (entry.first == QStringLiteral("inst-1")) {
            hasInst1 = true;
            break;
        }
    }
    check(hasInst1, "launcherInstancesForModule should list inst-1");
    check(spy.count() > 0, "enabledPluginsChanged should have fired during registration");

    int afterListCount = spy.count();

    // ===== startLauncherInstance: host grows, owner tracked =====
    bool started = registry.startLauncherInstance(module, QStringLiteral("inst-1"));
    check(started, "startLauncherInstance should succeed");
    check(hostInstanceCount() == baselineInstances + 1,
          "host instances should grow by one after start");
    check(hostHasInstance(QStringLiteral("inst-1")),
          "host should hold the started instance inst-1");

    auto *mockInstance = module->liveInstance(QStringLiteral("inst-1"));
    check(mockInstance != nullptr, "module should track the live mock instance");

    // ===== deleteInstance: dropped, stopped, no leak =====
    registry.deleteInstance(module, QStringLiteral("inst-1"));
    QApplication::processEvents();
    module->flush();
    check(hostInstanceCount() == baselineInstances,
          "host instance count should return to baseline after delete");
    check(!hostHasInstance(QStringLiteral("inst-1")),
          "host should no longer hold inst-1 after delete");
    check(mockInstance != nullptr && mockInstance->wasStopped(),
          "instance stop() should have been called on delete");
    check(module->deletedIds().contains(QStringLiteral("inst-1")),
          "owning module confirmed persisted deletion should have been invoked");
    check(spy.count() > afterListCount,
          "enabledPluginsChanged should fire again on deleteInstance");

    // ===== module registers outside startInstance: the device-connect shape =====
    smoke::SignalSpy removedSpy;
    QObject::connect(Host::get()->events(), &HostEvents::instanceRemoved, &removedSpy,
                     &smoke::SignalSpy::onSignal, qontrol::UNIQUE);

    auto *dev1 = module->registerLive(QStringLiteral("dev-1"), true, K_FINGERPRINT);
    check(hostInstance(QStringLiteral("dev-1")) == dev1,
          "host should hold an instance the module registered on its own");
    check(registry.instanceOwnerPluginId(QStringLiteral("dev-1")) == QStringLiteral("mock.plugin"),
          "a module-registered instance should be attributed to its plugin");

    module->registerLive(QStringLiteral("dev-1"), true, K_FINGERPRINT);
    check(hostInstance(QStringLiteral("dev-1")) == dev1,
          "re-registering a live id should be refused and keep the first instance");

    plugin.otherModule()->dropLive(QStringLiteral("dev-1"));
    check(hostHasInstance(QStringLiteral("dev-1")),
          "a module should not be able to drop an instance it does not own");
    check(dev1 != nullptr && !dev1->wasStopped(), "a refused drop should not stop the instance");

    // ===== signers(): implemented only, one entry per device =====
    module->registerLive(QStringLiteral("dev-3"), false); // inert signer
    module->startInstance(QStringLiteral("inst-2"));      // signer() == nullptr
    // The same seed on a second device, exposed by the other module.
    plugin.otherModule()->registerLive(QStringLiteral("dev-6"), true, K_FINGERPRINT);
    // A device the host can see but cannot use: it must still be listed, with its reason.
    module->registerLive(QStringLiteral("dev-7"), true, QString(), plugin::SignerState::Locked);

    auto signers = Host::get()->signers();
    check(signerIds() == QStringList{QStringLiteral("dev-1"), QStringLiteral("dev-6"),
                                     QStringLiteral("dev-7")},
          "signers() should list every implemented signer, ordered by instance id");
    check(signers.size() == 3 && signers.at(2).signer->state() == plugin::SignerState::Locked &&
              !signers.at(2).signer->stateDetail().isEmpty(),
          "an unusable device should be listed with its state and a reason, not hidden");
    check(signers.size() == 3 && signers.at(0).signer->state() == plugin::SignerState::Ready,
          "a usable device should report Ready");
    check(signers.size() == 3 && signers.at(0).signer->fingerprint() == K_FINGERPRINT &&
              signers.at(1).signer->fingerprint() == K_FINGERPRINT,
          "two devices sharing a fingerprint should both be listed");
    check(signers.size() == 3 && signers.at(0).module == module &&
              signers.at(1).module == plugin.otherModule(),
          "each entry should name the module that registered it");
    check(signers.size() == 3 && signers.at(0).signer == dev1->signer(),
          "the entry should route to that instance's own signer capability");

    // ===== the owning module drops it: the device-disconnect shape =====
    int removedBefore = removedSpy.count();
    module->dropLive(QStringLiteral("dev-1"));
    check(!hostHasInstance(QStringLiteral("dev-1")),
          "the owning module should be able to drop its instance");
    check(dev1 != nullptr && dev1->wasStopped(), "removeInstance should stop the instance");
    check(removedSpy.count() == removedBefore + 1, "instanceRemoved should fire on the drop");
    check(signerIds() == QStringList{QStringLiteral("dev-6"), QStringLiteral("dev-7")},
          "dropping one signer should leave the other entries alone");

    // ===== an instance retires itself =====
    auto *dev2 = module->registerLive(QStringLiteral("dev-2"), true);
    dev2->retire();
    check(!hostHasInstance(QStringLiteral("dev-2")),
          "retireInstance should unregister the instance");
    check(dev2->wasStopped(), "retireInstance should stop the instance");

    // ===== the host takes lifecycle calls from any thread =====
    auto *dev4 = module->createInstance(QStringLiteral("dev-4"), true);
    auto *dev5 = module->createInstance(QStringLiteral("dev-5"), true);
    QThread worker;
    smoke::ThreadedCaller caller4(module, dev4);
    smoke::ThreadedCaller caller5(module, dev5);
    caller4.moveToThread(&worker);
    caller5.moveToThread(&worker);
    worker.start();

    QMetaObject::invokeMethod(&caller4, "registerFromThread", Qt::QueuedConnection);
    check(waitForCalls(caller4, 1, 2000), "the worker should have returned from registerInstance");
    check(!hostHasInstance(QStringLiteral("dev-4")),
          "a cross-thread register must not land before the GUI thread runs it");
    check(waitForInstance(QStringLiteral("dev-4"), true, 2000),
          "the host should marshal a cross-thread register onto the GUI thread");

    QMetaObject::invokeMethod(&caller4, "dropFromThread", Qt::QueuedConnection);
    check(waitForCalls(caller4, 2, 2000), "the worker should have returned from removeInstance");
    check(hostHasInstance(QStringLiteral("dev-4")),
          "a cross-thread drop must not land before the GUI thread runs it");
    check(waitForInstance(QStringLiteral("dev-4"), false, 2000),
          "the host should marshal a cross-thread drop onto the GUI thread");
    check(dev4->wasStopped(), "a cross-thread drop should still stop the instance");

    QMetaObject::invokeMethod(&caller5, "registerFromThread", Qt::QueuedConnection);
    check(waitForInstance(QStringLiteral("dev-5"), true, 2000),
          "the host should marshal the second cross-thread register");
    QMetaObject::invokeMethod(&caller5, "retireFromThread", Qt::QueuedConnection);
    check(waitForCalls(caller5, 2, 2000), "the worker should have returned from retireInstance");
    check(hostHasInstance(QStringLiteral("dev-5")),
          "a cross-thread retire must not land before the GUI thread runs it");
    check(waitForInstance(QStringLiteral("dev-5"), false, 2000),
          "the host should marshal a cross-thread retire onto the GUI thread");
    check(dev5->wasStopped(), "a cross-thread retire should still stop the instance");

    worker.quit();
    worker.wait();

    // ===== polling: one flag per module, ORed into a global switch =====
    auto *other = plugin.otherModule();
    // Leave the second module holding no instance at all: the fan-out must still reach it,
    // because a module has to start its service before it has any device to show.
    other->dropLive(QStringLiteral("dev-6"));
    check(signerIds() == QStringList{QStringLiteral("dev-7")},
          "the second module should hold no signer once its device is dropped");

    smoke::SignalSpy pollingSpy;
    QObject::connect(Host::get()->events(), &HostEvents::signerPollingChanged, &pollingSpy,
                     &smoke::SignalSpy::onSignal, qontrol::UNIQUE);
    check(!module->polling() && !other->polling(), "nothing should poll before anyone asks");

    module->requestPolling(true);
    check(pollingSpy.count() == 1, "the first request should flip the global flag");
    check(module->polling() && other->polling(),
          "every module should be told to poll, including the one that asked for nothing");

    int callsBefore = module->pollingCalls();
    module->requestPolling(true);
    check(pollingSpy.count() == 1, "asking twice should not flip the flag again");
    check(module->pollingCalls() == callsBefore, "asking twice should not re-notify the modules");

    other->requestPolling(true);
    check(pollingSpy.count() == 1, "a second module asking should not flip an already-on flag");

    module->requestPolling(false);
    check(pollingSpy.count() == 1 && module->polling(),
          "polling should stay on while another module still wants it");

    other->requestPolling(false);
    check(pollingSpy.count() == 2, "the last request going away should flip the flag off");
    check(!module->polling() && !other->polling(), "every module should be told to stop");

    // ===== a module that answers before returning is still heard =====
    // The real built-in modules emit their list()/autoStart() results from inside the
    // call, so the answer lands before the registry has recorded the pending request.
    // The registry parks it and replays it; that replay must not be dropped.
    smoke::MockSyncPlugin syncPlugin;
    auto *syncModule = syncPlugin.module();
    registry.registerBuiltin(&syncPlugin);

    bool listed = false;
    for (const auto &entry : registry.launcherInstancesForModule(syncModule)) {
        if (entry.first == QStringLiteral("sync-1")) {
            listed = true;
            break;
        }
    }
    check(listed, "a module answering list() before it returns should still be listed");
    check(syncModule->started().contains(QStringLiteral("sync-auto")),
          "a module answering autoStart() before it returns should still be started");
    check(hostHasInstance(QStringLiteral("sync-auto")),
          "the auto-started instance should be registered with the host");

    // ===== refreshLauncher: a persisted instance shows up, nothing starts =====
    smoke::SignalSpy refreshSpy;
    QObject::connect(Host::get()->events(), &HostEvents::launcherRefreshRequested, &refreshSpy,
                     &smoke::SignalSpy::onSignal, qontrol::UNIQUE);
    int changedBefore = spy.count();

    syncModule->addListed(QStringLiteral("sync-2"), QStringLiteral("Sync Two"));
    Host::get()->refreshLauncher();
    check(refreshSpy.count() == 1, "refreshLauncher should emit launcherRefreshRequested once");
    check(launcherLists(registry, syncModule, QStringLiteral("sync-2")),
          "a relist should show the instance a module persisted since activation");
    check(spy.count() > changedBefore,
          "a relist answer should fire enabledPluginsChanged so the launcher refreshes");
    check(!syncModule->started().contains(QStringLiteral("sync-2")),
          "a relist should not start the newly listed instance");
    check(!hostHasInstance(QStringLiteral("sync-2")),
          "a relisted instance should be listed, not registered with the host");

    int changedBeforeFlush = spy.count();
    Host::get()->refreshLauncher();
    module->flush();
    check(!launcherLists(registry, module, QStringLiteral("inst-1")),
          "a deferred relist answer must not restore the account deleted earlier");
    check(spy.count() > changedBeforeFlush,
          "a deferred relist answer should be applied, not parked");

    auto *dev9 = module->createInstance(QStringLiteral("dev-9"), false);
    QThread refreshWorker;
    smoke::ThreadedCaller refreshCaller(module, dev9);
    refreshCaller.moveToThread(&refreshWorker);
    refreshWorker.start();

    int refreshBefore = refreshSpy.count();
    QMetaObject::invokeMethod(&refreshCaller, "refreshFromThread", Qt::QueuedConnection);
    check(waitForCalls(refreshCaller, 1, 2000),
          "the worker should have returned from refreshLauncher");
    check(refreshSpy.count() == refreshBefore,
          "a cross-thread refresh must not run before the GUI thread runs it");
    check(waitForSignals(refreshSpy, refreshBefore + 1, 2000),
          "the host should marshal a cross-thread refresh onto the GUI thread");
    check(refreshSpy.count() == refreshBefore + 1,
          "a cross-thread refresh should emit launcherRefreshRequested once");

    refreshWorker.quit();
    refreshWorker.wait();

    // ===== IKeys error kind: carried, defaulted, queued =====
    // deliverError never touches the module, so the bus needs none.
    keys::KeysBus bus(nullptr);
    smoke::ErrorKindSpy errorSpy;
    smoke::ErrorKindSpy legacySpy;
    QObject::connect(&bus, &IKeys::error, &errorSpy, &smoke::ErrorKindSpy::onError,
                     qontrol::UNIQUE);
    QObject::connect(&bus, &IKeys::error, &legacySpy, &smoke::ErrorKindSpy::onLegacyError,
                     qontrol::UNIQUE);

    bus.deliverError(7, QStringLiteral("plain"));
    check(errorSpy.count() == 1, "a two-argument deliverError should emit error once");
    check(errorSpy.kind() == plugin::SignError::Other, "the kind should default to Other");
    check(errorSpy.reqId() == std::optional<ReqId>(7), "the request id should be carried");
    check(legacySpy.count() == 1, "a two-argument slot should still receive error");

    bus.deliverError(8, QStringLiteral("lost"), plugin::SignError::MusigSession);
    check(errorSpy.count() == 2 && errorSpy.kind() == plugin::SignError::MusigSession,
          "the MusigSession kind should be carried");
    check(errorSpy.message() == QStringLiteral("lost"), "the message should be carried");

    QThread errorThread;
    smoke::ErrorKindSpy queuedSpy;
    queuedSpy.moveToThread(&errorThread);
    errorThread.start();
    QObject::connect(&bus, &IKeys::error, &queuedSpy, &smoke::ErrorKindSpy::onError,
                     qontrol::UNIQUE);
    bus.deliverError(9, QStringLiteral("queued"), plugin::SignError::MusigSession);
    QElapsedTimer errorTimer;
    errorTimer.start();
    while (queuedSpy.count() < 1 && errorTimer.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    errorThread.quit();
    errorThread.wait();
    check(queuedSpy.count() == 1, "a queued receiver should get error once");
    check(queuedSpy.kind() == plugin::SignError::MusigSession,
          "the kind should survive a queued connection");

    // ===== HwiSigner error kind =====
    // deliver() of an error never touches the module, so the signer needs none.
    HwiDevice smokeDevice;
    smokeDevice.id = rust::String("smoke-ledger");
    smokeDevice.kind = rust::String("ledger");
    smokeDevice.fingerprint = rust::String(K_FINGERPRINT.toStdString());
    smokeDevice.version = rust::String("2.4.6");
    smokeDevice.state = HwiDeviceState::Supported;
    smokeDevice.detail = rust::String("");
    HwiSigner smokeSigner(nullptr, smokeDevice);
    smoke::ErrorKindSpy signerSpy;
    QObject::connect(&smokeSigner, &ISigner::error, &signerSpy, &smoke::ErrorKindSpy::onError,
                     qontrol::UNIQUE);

    HwiEvent lostEvent;
    lostEvent.flag = HwiEventFlag::Error;
    lostEvent.req_id = 11;
    lostEvent.error = rust::String("lost");
    lostEvent.error_kind = HwiErrorKind::MusigSession;
    smokeSigner.deliver(lostEvent);
    check(signerSpy.count() == 1, "a device error should emit error once");
    check(signerSpy.kind() == plugin::SignError::MusigSession,
          "a lost MuSig2 session should reach ISigner::error as MusigSession");
    check(signerSpy.reqId() == std::optional<ReqId>(11), "the device request id should be carried");
    check(signerSpy.message() == QStringLiteral("lost"), "the device message should be carried");

    HwiEvent otherEvent = lostEvent;
    otherEvent.error_kind = HwiErrorKind::Other;
    smokeSigner.deliver(otherEvent);
    check(signerSpy.count() == 2 && signerSpy.kind() == plugin::SignError::Other,
          "any other device error should reach ISigner::error as Other");
    check(HwiSigner::provesRegistrationByHmac(QStringLiteral("ledger")) &&
              HwiSigner::provesRegistrationByHmac(QStringLiteral("ledger-simulator")),
          "a Ledger's stored proof should answer its registration check");
    check(!HwiSigner::provesRegistrationByHmac(QStringLiteral("coldcard")) &&
              !HwiSigner::provesRegistrationByHmac(QStringLiteral("coldcard-simulator")),
          "a Coldcard on the same seed should still be asked");
    check(HwiSigner::registersBeforeApproval(QStringLiteral("coldcard")) &&
              HwiSigner::registersBeforeApproval(QStringLiteral("coldcard-simulator")) &&
              !HwiSigner::registersBeforeApproval(QStringLiteral("ledger")) &&
              !HwiSigner::registersBeforeApproval(QStringLiteral("jade")),
          "only a Coldcard registration should wait for its approval to be listed");
    check(keys::deviceName(QStringLiteral("coldcard-simulator"), QString()) ==
                  QStringLiteral("Coldcard Simulator") &&
              keys::deviceName(QStringLiteral("ledger-simulator"), QString()) ==
                  QStringLiteral("Ledger Simulator"),
          "both simulators should show their brand name");
    // ===== musig module lists only musig configs =====
    QString spAccount = QStringLiteral("smoke-sp");
    QString musigAccount = QStringLiteral("smoke-musig");
    ::new_config(rust::String(spAccount.toStdString()), Network::Regtest,
                 rust::String("sp-descriptor"), rust::String(""), rust::String(""), 546,
                 rust::String("sp"))
        ->to_file();
    ::new_config(rust::String(musigAccount.toStdString()), Network::Regtest,
                 rust::String("musig-descriptor"), rust::String(""), rust::String(""), 546,
                 rust::String("musig"))
        ->to_file();

    MusigModule musigModule;
    ModuleSpy musigSpy;
    QObject::connect(&musigModule, &IModule::instances, &musigSpy, &ModuleSpy::onInstances,
                     qontrol::UNIQUE);
    QObject::connect(&musigModule, &IModule::error, &musigSpy, &ModuleSpy::onError,
                     qontrol::UNIQUE);

    auto listRequest = musigModule.list();
    check(musigSpy.waitForInstances(listRequest, 10000),
          "the musig module answers the requested asynchronous config listing");
    QList<QPair<QString, QString>> musigListed{{musigAccount, musigAccount}};
    check(musigSpy.instances() == musigListed,
          "the musig module should list exactly the musig account, no sp account, no create entry");

    int musigInstancesBefore = hostInstanceCount();
    musigModule.startInstance(spAccount);
    check(musigSpy.errors() == 1, "starting an sp account from the musig module should error");
    check(hostInstanceCount() == musigInstancesBefore && !hostHasInstance(spAccount),
          "starting an sp account from the musig module should register no instance");

    check(::delete_sp_account(rust::String(spAccount.toStdString())).empty(),
          "the sp config should delete");
    check(::delete_musig_account(rust::String(musigAccount.toStdString())).empty(),
          "the musig config should delete");

    // ===== musig creator lists its create entry only =====
    MusigCreatorModule creatorModule;
    ModuleSpy creatorSpy;
    QObject::connect(&creatorModule, &IModule::instances, &creatorSpy, &ModuleSpy::onInstances,
                     qontrol::UNIQUE);
    QObject::connect(&creatorModule, &IModule::error, &creatorSpy, &ModuleSpy::onError,
                     qontrol::UNIQUE);

    check(creatorModule.meta().value(QStringLiteral("id")) == QStringLiteral("musig.creator"),
          "the musig creator module id should be musig.creator");
    creatorModule.list();
    check(creatorSpy.instances().size() == 2 &&
              creatorSpy.instances().first().first == QStringLiteral("musig.create") &&
              creatorSpy.instances().last().first == QStringLiteral("musig.restore"),
          "the musig creator should list exactly the musig.create and musig.restore entries");
    check(PluginRegistry::isCreatorInstanceId(QStringLiteral("musig.create")),
          "musig.create should be a creator instance id");
    // musig.create itself opens a modal, so only the unknown id is started here.
    creatorModule.startInstance(QStringLiteral("nope"));
    check(creatorSpy.errors() == 1, "starting an unknown id from the musig creator should error");

    smoke::feePersistenceSmoke();

    if (gOk) {
        std::fprintf(stdout, "host smoke: OK\n");
        return 0;
    }
    std::fprintf(stderr, "host smoke: FAILED\n");
    return 1;
}

#include "host_smoke.moc"
