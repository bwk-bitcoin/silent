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
#include <QElapsedTimer>
#include <QEventLoop>
#include <QList>
#include <QString>
#include <QStringList>
#include <QThread>

#include <Qontrol>
#include <common.h>

#include <host/Host.h>
#include <host/PluginRegistry.h>
#include <interfaces/instance.h>
#include <interfaces/module.h>
#include <interfaces/signer.h>

#include "MockPlugin.h"

namespace {

bool g_ok = true;

// Shared by two devices holding the same seed, exposed by different modules.
const QString K_FINGERPRINT = QStringLiteral("a4f9c821");

void check(bool cond, const char *msg) {
    if (!cond) {
        g_ok = false;
        std::fprintf(stderr, "FAIL: %s\n", msg);
    }
}

auto hostInstanceCount() -> int {
    return Host::get()->instances().size();
}

auto hostInstance(const QString &id) -> IInstance * {
    for (auto *instance : Host::get()->instances()) {
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

auto launcherLists(const PluginRegistry &registry, const IModule *module, const QString &id)
    -> bool {
    for (const auto &entry : registry.launcherInstancesForModule(module)) {
        if (entry.first == id) {
            return true;
        }
    }
    return false;
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    QApplication app(argc, argv);

    // Initialise the host singleton before the registry connects to its events.
    Host::get();

    smoke::MockPlugin plugin;
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
    check(hostInstanceCount() == baselineInstances,
          "host instance count should return to baseline after delete");
    check(!hostHasInstance(QStringLiteral("inst-1")),
          "host should no longer hold inst-1 after delete");
    check(mockInstance != nullptr && mockInstance->wasStopped(),
          "instance stop() should have been called on delete");
    check(module->deletedIds().contains(QStringLiteral("inst-1")),
          "owning module deleteInstance should have been invoked");
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
    check(launcherLists(registry, module, QStringLiteral("inst-1")),
          "a deferred relist answer should still list inst-1");
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

    if (g_ok) {
        std::fprintf(stdout, "host smoke: OK\n");
        return 0;
    }
    std::fprintf(stderr, "host smoke: FAILED\n");
    return 1;
}
