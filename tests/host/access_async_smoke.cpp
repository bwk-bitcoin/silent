#include "access_async_smoke.h"

#include "AppController.h"
#include "MainWindow.h"
#include "PluginBundleFixture.h"
#include "access_history_refresh_smoke.h"
#include "account/AccountModule.h"
#include "authorization_lifetime_smoke.h"
#include "grant_freshness_smoke.h"
#include "host/Host.h"
#include "host/PluginAccess.h"
#include "host/PluginRegistry.h"
#include "interfaces/module.h"
#include "listing_reentrancy_smoke.h"
#include "modules/keys/KeysModule.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/sp/SpPlugin.h"
#include "registry_async_errors.h"
#include "views/modals/PluginAccessPrompt.h"
#include "views/modals/appsettings/AccessFlow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <common.h>
#include <cstdio>
#include <dlfcn.h>
#include <memory>
#include <silent.h>
#include <sys/file.h>

namespace smoke {
namespace {
constexpr int TICK_MS = 5;
// Ticks the GUI runs while the lock is held before the driver acts.
constexpr int HELD_TICKS = 40;
constexpr int SETTLE_MS = 2000;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("access async: %s", message);
    }
}

auto openPrompt() -> modal::PluginAccessPrompt * {
    for (auto *widget : QApplication::topLevelWidgets()) {
        auto *prompt = qobject_cast<modal::PluginAccessPrompt *>(widget);
        if (prompt != nullptr && prompt->isVisible()) {
            return prompt;
        }
    }
    return nullptr;
}

auto stateAnswer(const QString &id, const QString &hash) -> PluginGrant {
    return ::app_plugin_answers(rust::String(id.toStdString()), rust::String(hash.toStdString()))
        .state;
}

auto fixtureModule(PluginRegistry *registry) -> IModule * {
    for (auto *plugin : registry->plugins()) {
        for (auto *module : plugin->modules()) {
            if (module->meta().value(QStringLiteral("id")) == QStringLiteral("removal.module")) {
                return module;
            }
        }
    }
    qFatal("access async: the native fixture module is not loaded");
}

// Lets the jobs left running after a withdrawal finish against the released lock.
void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < SETTLE_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
}

void waitForScan(PluginRegistry *registry) {
    QElapsedTimer timer;
    timer.start();
    while (registry->pluginStateLoading() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
    require(!registry->pluginStateLoading(), "the checked registry snapshot completes");
    require(registry->pluginRemovalReadError().isEmpty(), "the registry snapshot is readable");
}

void saveStateAnswer(const QString &id, const QString &hash, PluginGrant grant) {
    auto request = ::app_plugin_access_request(rust::String(id.toStdString()),
                                               rust::String(hash.toStdString()));
    require(request.is_ok, "the fixture reads its current storage epoch");
    request.access.state = grant;
    require(::app_set_plugin_access(request.access, request.storage_epoch).empty(),
            "the fixture state grant saves against its current epoch");
}

auto prepareFixture(MainWindow *window) -> IModule * {
    QFile fixture(QStringLiteral(SILENT_REMOVAL_FIXTURE));
    require(fixture.open(QIODevice::ReadOnly), "the native fixture library opens");
    auto bytes = fixture.readAll();
    auto id = QStringLiteral("removal.fixture");
    auto hash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    require(installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "the native fixture installs");
    saveStateAnswer(id, hash, PluginGrant::Allowed);
    auto *controller = AppController::get();
    controller->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    controller->registerBuiltinPlugin(std::make_unique<MusigPlugin>());
    controller->registerBuiltinPlugin(std::make_unique<keys::KeysModule>());
    controller->initState();
    controller->start(window);
    window->show();
    auto *registry = controller->pluginRegistry();
    waitForScan(registry);
    RegistryJobLifetimeProbe lifetime(registry);
    lifetime.arm();
    AppStateLock enableLock;
    enableLock.hold();
    require(registry->setEnabled(id, true), "the native fixture enable is admitted");
    enableLock.release();
    QElapsedTimer timer;
    timer.start();
    while (registry->pluginEnablePending(id) && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
    require(registry->isPluginEnabled(id), "the native fixture enable completes");
    require(lifetime.observed(),
            "native enable safely consumes a destroyed state job's owned result");
    require(registry->pluginHash(id) == hash,
            "destroying the state job preserves the exact loaded library payload hash");
    std::fprintf(stdout, "State job deleted inside native grant wait; rescan serialized: OK\n");
    settle();
    return fixtureModule(registry);
}

class ListingOwner final : public account::AccountModule {
public:
    ListingOwner() : AccountModule("sp", ::sp_account_delete_check, ::delete_sp_account) {
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {};
    }

    auto autoStart() -> ReqId override {
        return 0;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }
};

void closeWaitingOwners(PluginRegistry *registry) {
    PluginRegistry temporaryRegistry;
    temporaryRegistry.rescanDiscoveredPlugins();
    ListingOwner temporaryModule;
    require(temporaryModule.list() != 0, "a temporary owner starts real config discovery");
    auto *flow = new modal::AccessFlow(registry, QStringLiteral("removal.fixture"));
    flow->start();
    delete flow;
}

void staleSaveRefused(const QString &id, const QString &hash) {
    auto request = ::app_plugin_access_request(rust::String(id.toStdString()),
                                               rust::String(hash.toStdString()));
    require(request.is_ok, "the stale-save fixture reads current answers");
    modal::PluginAccessPrompt prompt(QStringLiteral("Removal fixture"), QStringLiteral("1.0.0"),
                                     request.access, request.storage_epoch + 1,
                                     request.grant_version, {.state = true});
    prompt.onDenyClicked();
    prompt.exec();
    require(!prompt.saved() && prompt.error().contains(QStringLiteral("removed")),
            "a stale-epoch async save is refused with its backend error");
    require(stateAnswer(id, hash) == PluginGrant::Allowed,
            "a refused Deny save does not change the durable Allow answer");
}

auto storageReply(uint64_t session, const QJsonObject &request) -> QJsonObject {
    auto response = ::storage_request(
        session, rust::String(QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString()));
    auto document = QJsonDocument::fromJson(
        QByteArray(response.data(), static_cast<qsizetype>(response.size())));
    require(document.isObject(), "the real storage backend answers a JSON object");
    return document.object();
}

void checkRemovalMarker(uint64_t session, const QString &handle, const QString &path) {
    require(QFile::exists(path), "atomic removal retains its encrypted migration marker");
    auto listed = storageReply(session, {{QStringLiteral("id"), QStringLiteral("empty-marker")},
                                         {QStringLiteral("op"), QStringLiteral("list")},
                                         {QStringLiteral("storage"), handle}});
    require(listed.value(QStringLiteral("error")) == QString() &&
                listed.value(QStringLiteral("entries")).isArray() &&
                listed.value(QStringLiteral("entries")).toArray().isEmpty(),
            "the decrypted marker has no private entries");
    auto migration =
        storageReply(session, {{QStringLiteral("id"), QStringLiteral("fenced-marker")},
                               {QStringLiteral("op"), QStringLiteral("migrate")},
                               {QStringLiteral("storage"), handle},
                               {QStringLiteral("path"), QStringLiteral("must-not-migrate")}});
    require(migration.value(QStringLiteral("error")) == QString() &&
                migration.value(QStringLiteral("migrated")).isBool() &&
                !migration.value(QStringLiteral("migrated")).toBool() &&
                migration.value(QStringLiteral("reason")) == QStringLiteral("completed"),
            "the retained marker is already migrated and cannot import legacy data again");
}
} // namespace

void AppStateLock::hold() {
    m_file.setFileName(QDir::homePath() + QStringLiteral("/.silent/app.lock"));
    require(m_file.open(QIODevice::ReadWrite), "the real app.lock opens");
    require(::flock(m_file.handle(), LOCK_EX) == 0, "the test holds the real app.lock");
}

void AppStateLock::release() {
    require(::flock(m_file.handle(), LOCK_UN) == 0, "the test releases the real app.lock");
    m_file.close();
}

AccessDriver::AccessDriver(HostImpl *host, IModule *module, AppStateLock *lock)
    : m_host(host),
      m_module(module),
      m_lock(lock) {
    connect(&m_timer, &QTimer::timeout, this, &AccessDriver::onTick, qontrol::UNIQUE);
}

void AccessDriver::start(Step step) {
    m_step = step;
    m_ticks = 0;
    m_prompted = false;
    m_held_during_save = false;
    m_saved_at = 0;
    m_timer.start(TICK_MS);
}

void AccessDriver::stop() {
    m_timer.stop();
}

void AccessDriver::holdPublication(PluginRegistry *registry) {
    m_registry = registry;
    connect(registry, &PluginRegistry::pluginStopFinished, this, &AccessDriver::onStopped,
            qontrol::UNIQUE);
    start(Step::Publication);
}

void AccessDriver::onStopped([[maybe_unused]] const QString &plugin_id, bool removed,
                             const QString &error) {
    m_removed = removed;
    m_stop_error = error;
}

void AccessDriver::onAbandonedStorageOpened([[maybe_unused]] uint64_t revision) {
    auto *job = qobject_cast<pluginaccess::Job *>(sender());
    require(job != nullptr && job->takeSession() == 0,
            "a released open job revokes any late session without handing it out");
    m_abandoned_session_checked = true;
}

// Allow is clicked with the lock held: the save waits on it while the GUI keeps running,
// the prompt open and its buttons disabled, then the lock is released.
void AccessDriver::heldSave() {
    auto *prompt = openPrompt();
    if (prompt == nullptr) {
        return;
    }
    m_prompted = true;
    if (!prompt->answered()) {
        m_lock->hold();
        prompt->onAllowClicked();
        m_saved_at = m_ticks;
        return;
    }
    if (m_saved_at != 0 && m_ticks >= m_saved_at + HELD_TICKS) {
        m_held_during_save = !prompt->saved() && prompt->error().isEmpty();
        m_saved_at = 0;
        m_lock->release();
    }
}

void AccessDriver::onTick() {
    ++m_ticks;
    switch (m_step) {
    case Step::HeldRead:
        if (m_ticks == HELD_TICKS) {
            m_host->setCallerClosing(m_module, true);
        }
        break;
    case Step::Prompt:
        if (!m_prompted && openPrompt() != nullptr) {
            m_prompted = true;
            m_host->setCallerClosing(m_module, true);
        }
        break;
    case Step::HeldSave:
        heldSave();
        break;
    case Step::Publication:
        if (m_ticks == HELD_TICKS) {
            m_registry->removePlugin(QStringLiteral("removal.fixture"));
        }
        break;
    }
}

void accessAsyncSmoke() {
    MainWindow window;
    auto *module = prepareFixture(&window);
    accessHistoryRefreshSmoke();
    auto id = QStringLiteral("removal.fixture");
    auto *controller = AppController::get();
    auto *registry = controller->pluginRegistry();
    auto hash = registry->pluginHash(id);
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the host attributes callers");
    saveStateAnswer(id, hash, PluginGrant::Unasked);
    require(stateAnswer(id, hash) == PluginGrant::Unasked, "the fresh library has no answer");

    AppStateLock lock;
    AccessDriver driver(host, module, &lock);

    driver.start(AccessDriver::Step::HeldRead);
    lock.hold();
    auto access = host->requestAccess(module, {.state = true});
    require(driver.ticks() >= HELD_TICKS,
            "the GUI kept running while the grant read waited on the held app.lock");
    require(!access.account && !access.keys && !access.state,
            "a request withdrawn while its read waited grants nothing");
    require(host->callerFramesIdle(module), "the withdrawn request left no active frame");
    lock.release();
    host->setCallerClosing(module, false);
    settle();

    driver.start(AccessDriver::Step::Prompt);
    access = host->requestAccess(module, {.state = true});
    require(driver.prompted(), "the real prompt opened for the unasked Saved Data grant");
    require(!access.state, "a prompt withdrawn by closing grants nothing");
    require(openPrompt() == nullptr, "closing withdrew the prompt");
    require(stateAnswer(id, hash) == PluginGrant::Unasked, "a withdrawn prompt saves nothing");
    require(host->callerFramesIdle(module), "the withdrawn prompt left no active frame");
    host->setCallerClosing(module, false);

    driver.start(AccessDriver::Step::HeldSave);
    access = host->requestAccess(module, {.state = true});
    require(driver.heldDuringSave(),
            "Allow waited unsaved, with the GUI running, while its save was held on app.lock");
    require(access.state, "the saved Allow reaches the current caller");
    require(stateAnswer(id, hash) == PluginGrant::Allowed, "the Allow answer is really saved");
    require(host->callerFramesIdle(module), "the answered request left no active frame");
    driver.stop();
    listingReentrySmoke();
    std::fprintf(stdout, "Reentrant account list completions: OK\n");
    staleSaveRefused(id, hash);
    driver.stop();
    authorizationLifetimeSmoke(host, module);
    registryCheckpointFailureSmoke();
    registryEnableRollbackFailureSmoke();
    registryEnableRollbackReadErrorSmoke();
    registryStaleScanErrorsSmoke();
    registryAccountDeletionRefreshSmoke();
    std::fprintf(stdout,
                 "Registry checkpoint failure, Disable rollback and stale scan intent: OK\n");
    grantFreshnessSmoke(host, module);
    std::fprintf(stdout, "Nested grant revocation, expiry, closing and shutdown: OK\n");
    settle();
}

void publicationHeartbeatSmoke() {
    using Action = void (*)();
    using Held = bool (*)();
    auto arm = reinterpret_cast<Action>(dlsym(RTLD_DEFAULT, "silentTestFsyncArm"));
    auto held = reinterpret_cast<Held>(dlsym(RTLD_DEFAULT, "silentTestFsyncHeld"));
    auto release = reinterpret_cast<Action>(dlsym(RTLD_DEFAULT, "silentTestFsyncRelease"));
    require(arm != nullptr && held != nullptr && release != nullptr,
            "run with LD_PRELOAD pointing to libfsync_hold_shim.so");
    MainWindow window;
    QPointer<IModule> module = prepareFixture(&window);
    auto *controller = AppController::get();
    auto *registry = controller->pluginRegistry();
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    auto id = QStringLiteral("removal.fixture");
    auto session = ::storage_session_open(
        rust::String(id.toStdString()), rust::String(registry->pluginHash(id).toStdString()), true);
    require(session != 0, "an authenticated external writer opens before removal");
    auto opened = ::storage_request(session, R"({"id":"open","op":"open","storage":"private"})");
    auto handle =
        QJsonDocument::fromJson(QByteArray(opened.data(), static_cast<qsizetype>(opened.size())))
            .object()
            .value(QStringLiteral("storage"))
            .toString();
    require(!handle.isEmpty(), "the accepted writer owns the real private tree");
    QJsonObject write{{QStringLiteral("id"), QStringLiteral("seed-publication")},
                      {QStringLiteral("op"), QStringLiteral("write")},
                      {QStringLiteral("storage"), handle},
                      {QStringLiteral("path"), QStringLiteral("heartbeat")},
                      {QStringLiteral("data"), QStringLiteral("cHVibGljYXRpb24=")},
                      {QStringLiteral("mode"), QStringLiteral("upsert")}};
    auto seeded = ::storage_request(
        session, rust::String(QJsonDocument(write).toJson(QJsonDocument::Compact).toStdString()));
    require(
        QJsonDocument::fromJson(QByteArray(seeded.data(), static_cast<qsizetype>(seeded.size())))
            .object()
            .value(QStringLiteral("error"))
            .toString()
            .isEmpty(),
        "the pre-removal private tree has a durable seeded file");
    auto path = QDir::homePath() + QStringLiteral("/.silent/removal.fixture/tree.bip138");
    QFile original(path);
    require(original.open(QIODevice::ReadOnly), "the existing private tree is readable");
    auto before = original.readAll();
    original.close();
    write[QStringLiteral("id")] = QStringLiteral("held-publication");
    auto message = QJsonDocument(write).toJson(QJsonDocument::Compact);
    auto answer = std::make_shared<QByteArray>();
    arm();
    auto *writer = QThread::create([session, message, answer] {
        auto response = ::storage_request(session, rust::String(message.toStdString()));
        *answer = QByteArray(response.data(), static_cast<qsizetype>(response.size()));
    });
    writer->start();
    QElapsedTimer timer;
    timer.start();
    while (!held() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
    require(held(), "the accepted writer reached fsync inside real publication");
    QFile staging(QDir::homePath() + QStringLiteral("/.silent/removal.fixture/tree.tmp"));
    require(staging.size() > 0, "ciphertext was written before the held fsync");
    require(writer->isRunning(), "the accepted writer has not finished its held publication");
    closeWaitingOwners(registry);
    AppStateLock unusedLock;
    AccessDriver driver(host, module, &unusedLock);
    auto *abandoned = new pluginaccess::Job;
    QObject::connect(abandoned, &pluginaccess::Job::storageOpened, &driver,
                     &AccessDriver::onAbandonedStorageOpened, qontrol::UNIQUE);
    abandoned->openStorage(1, id, registry->pluginHash(id), true);
    abandoned->release();
    QObject::connect(&driver, SIGNAL(releaseWorker()), module.data(), SLOT(releaseWorker()),
                     qontrol::UNIQUE);
    registry->rescanDiscoveredPlugins();
    controller->listAccounts();
    registry->relistModules();
    driver.holdPublication(registry);
    auto access = host->requestAccess(module, {});
    require(driver.ticks() >= HELD_TICKS, "40 real GUI timer heartbeats ran during the held fsync");
    require(!access.state && !access.account && !access.keys,
            "closing cancels the grant read waiting behind the accepted writer");
    require(registry->pluginStopPending(id), "removal remains pending behind publication");
    require(module != nullptr && writer->isRunning(), "no unload occurs with accepted work held");
    require(original.open(QIODevice::ReadOnly) && original.readAll() == before,
            "removal did not delete or replace the original ciphertext during fsync");
    original.close();
    std::fprintf(stdout,
                 "Held inside fsync: %d GUI heartbeats, removal pending, ciphertext retained\n",
                 driver.ticks());
    release();
    require(writer->wait(5000), "the accepted writer finishes after fsync is released");
    delete writer;
    require(QJsonDocument::fromJson(*answer)
                .object()
                .value(QStringLiteral("error"))
                .toString()
                .isEmpty(),
            "the accepted writer publishes before removal can advance");
    emit driver.releaseWorker();
    timer.restart();
    while (registry->pluginStopPending(id) && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
    require(!registry->pluginStopping(id) && registry->pluginStopError(id).isEmpty(),
            "removal completes after the writer and native worker drain");
    require(driver.removed() && driver.stopError().isEmpty(),
            "the registry explicitly reports successful destructive removal");
    require(module == nullptr, "native module is destroyed before removal completion");
    auto inspector = ::storage_session_open(rust::String(id.toStdString()), rust::String(), false);
    auto inspected =
        ::storage_request(inspector, R"({"id":"open","op":"open","storage":"private"})");
    auto inspectionHandle =
        QJsonDocument::fromJson(
            QByteArray(inspected.data(), static_cast<qsizetype>(inspected.size())))
            .object()
            .value(QStringLiteral("storage"))
            .toString();
    require(!inspectionHandle.isEmpty(), "the private tree reopens after removal");
    checkRemovalMarker(inspector, inspectionHandle, path);
    for (auto file : {QStringLiteral("heartbeat"), QStringLiteral("shutdown-flush")}) {
        QJsonObject inspect{{QStringLiteral("id"), QStringLiteral("inspect")},
                            {QStringLiteral("op"), QStringLiteral("inspect")},
                            {QStringLiteral("storage"), inspectionHandle},
                            {QStringLiteral("path"), file}};
        auto result = ::storage_request(
            inspector,
            rust::String(QJsonDocument(inspect).toJson(QJsonDocument::Compact).toStdString()));
        require(QJsonDocument::fromJson(
                    QByteArray(result.data(), static_cast<qsizetype>(result.size())))
                        .object()
                        .value(QStringLiteral("error")) == QStringLiteral("not_found"),
                "the empty migration-fenced tree contains neither accepted nor final-write data");
    }
    ::storage_session_revoke(inspector);
    require(!::plugin_library(rust::String(id.toStdString())).is_ok,
            "the installed bundle is deleted");
    auto stale = ::storage_request(session, R"({"id":"stale","op":"open","storage":"private"})");
    require(QJsonDocument::fromJson(QByteArray(stale.data(), static_cast<qsizetype>(stale.size())))
                    .object()
                    .value(QStringLiteral("error")) == QStringLiteral("revoked"),
            "the pre-removal external session cannot recreate deleted state");
    ::storage_session_revoke(session);
    settle();
    require(driver.abandonedSessionChecked(), "the abandoned open worker completes after release");
}

} // namespace smoke
