#include "registry_async_errors.h"

#include "AppController.h"
#include "MockPlugin.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QPluginLoader>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <common.h>
#include <cstdint>
#include <optional>
#include <silent.h>

// Inspect transitions without injecting filesystem failures into shared app state.
#define private public
#define protected public
#include "host/PluginRegistry.h"
#undef protected
#undef private

namespace smoke {
namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("registry async errors: %s", message);
    }
}

class RegistryErrorModule final : public IModule {
public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("registry.errors.module")}};
    }

    auto list() -> ReqId override {
        return 0;
    }

    auto autoStart() -> ReqId override {
        return 0;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }

    void shutdown(ReqId req_id) override {
        ++m_shutdowns;
        m_request = req_id;
    }

    void complete(const QString &error = {}) {
        emit shutdownFinished(m_request, error);
    }

    [[nodiscard]] auto shutdowns() const -> int {
        return m_shutdowns;
    }

    [[nodiscard]] auto request() const -> ReqId {
        return m_request;
    }

private:
    int m_shutdowns = 0;
    ReqId m_request = 0;
};

class RegistryListingModule final : public IModule {
public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("registry.listing.module")}};
    }

    auto list() -> ReqId override {
        return ++m_request;
    }

    auto autoStart() -> ReqId override {
        return 0;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }

private:
    ReqId m_request = 0;
};

class RegistryErrorFixture {
public:
    RegistryErrorFixture() {
        // Retire the constructor's read, then park subsequent jobs for explicit completions.
        auto *initial = m_registry.m_state_job.data();
        require(initial != nullptr, "the constructor requests a checked snapshot");
        initial->wait();
        QObject::disconnect(initial, nullptr, &m_registry, nullptr);
        QCoreApplication::removePostedEvents(&m_registry, QEvent::MetaCall);
        delete initial;
        m_registry.m_state_job = new PluginStateJob(PluginStateAction::Scan, {}, 0, 0, false);
        m_registry.m_scan_requested = false;
        m_registry.m_reload_requested = false;
        m_registry.m_state_deadline.stop();
        m_registry.m_state_loaded = true;
        m_registry.m_removal_read_error.clear();
        auto &record = m_registry.m_records[m_id];
        record.id = m_id;
        record.external = true;
        record.discovered = true;
    }

    ~RegistryErrorFixture() {
        delete m_registry.m_state_job.data();
        m_registry.m_state_job = nullptr;
    }

    QString m_id = QStringLiteral("registry.errors.fixture");
    PluginRegistry m_registry;
};

void deliverShutdownReplies() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}

void checkpointFailure(bool remove, bool reply_before_checkpoint, bool refuse) {
    RegistryErrorModule module;
    RegistryErrorFixture fixture;
    auto &registry = fixture.m_registry;
    auto id = fixture.m_id;
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the host is initialized before registry probes");
    auto instanceId = id + QStringLiteral(".instance");
    MockInstance instance(instanceId);
    registry.m_records[id].modules = {&module};
    registry.m_records[id].enabled = true;
    registry.m_module_owner.insert(&module, {.plugin_id = id,
                                             .module_id = QStringLiteral("registry.errors.module"),
                                             .plugin_hash = {},
                                             .external = true});
    auto *applicationRegistry = AppController::get()->pluginRegistry();
    require(applicationRegistry != nullptr,
            "the application registry is initialized before registry probes");
    require(!applicationRegistry->m_records.contains(id) &&
                !applicationRegistry->m_module_owner.contains(&module),
            "the fixture does not replace application registry ownership");
    applicationRegistry->m_records.insert(id, registry.m_records.value(id));
    applicationRegistry->m_module_owner.insert(&module, registry.m_module_owner.value(&module));
    require(applicationRegistry->caller(&module).has_value(),
            "the real host recognizes the external fixture caller");
    host->registerInstance(instanceId, &instance, &module);
    QCoreApplication::sendPostedEvents(host, QEvent::MetaCall);
    require(host->instanceOwner(instanceId) == &module,
            "the real host admits the fixture instance before shutdown");
    require(registry.instanceOwnerPluginId(instanceId) == id,
            "the local registry attributes the admitted instance through host events");
    host->enterFrame(&module);
    registry.beginPluginStop(id, remove);
    registry.advancePluginStops();
    require(module.shutdowns() == 0, "an active native frame delays shutdown");
    require(registry.pluginStopPending(id), "the accepted action remains pending");
    host->leaveFrame(&module);
    registry.advancePluginStops();
    require(instance.wasStopped(), "acceptance withdraws runtime before checkpoint completion");
    require(module.shutdowns() == 1, "cooperative shutdown starts before checkpoint completion");
    auto request = module.request();
    auto shutdownError = refuse ? QStringLiteral("fixture refuses shutdown") : QString();
    if (reply_before_checkpoint) {
        module.complete(shutdownError);
        deliverShutdownReplies();
        registry.advancePluginStops();
        require(registry.pluginStopPending(id),
                "a pending checkpoint prevents retry after shutdown");
        require(!registry.pluginDisableRetryable(id), "no retry races the pending checkpoint");
    }
    registry.onPluginStopPrepared(id, QStringLiteral("fixture checkpoint failure"));
    require(registry.pluginStopError(id).contains(QStringLiteral("fixture checkpoint failure")),
            "checkpoint failures are surfaced");
    if (!reply_before_checkpoint) {
        require(registry.pluginStopPending(id),
                "checkpoint failure still waits for module replies");
        module.complete(shutdownError);
        deliverShutdownReplies();
    }
    registry.advancePluginStops();
    require(!registry.pluginStopPending(id), "all completions leave a retryable failed stop");
    require(registry.pluginDisableRetryable(id) == !remove, "Disable retry stays non-destructive");
    require(!registry.m_stops.value(id).callers_dropped,
            "failed checkpoints retain final-write handles");
    require(registry.m_records.value(id).modules.contains(&module),
            "failed checkpoints retain modules");
    require(!registry.m_records.value(id).removal_phase.has_value(),
            "failed saves do not invent a journal phase");
    if (refuse) {
        require(registry.pluginStopError(id).contains(shutdownError),
                "shutdown and checkpoint errors survive either completion order");
    }
    registry.beginPluginStop(id, remove);
    registry.onPluginStopPrepared(id, {});
    registry.advancePluginStops();
    require(module.shutdowns() == 2 && module.request() != request,
            "retry has a fresh correlated shutdown request");
    emit module.shutdownFinished(request, {});
    deliverShutdownReplies();
    require(registry.m_stops.value(id).pending.contains(&module),
            "stale shutdown replies cannot finish a retry");
    module.complete();
    deliverShutdownReplies();
    registry.advancePluginStops();
    require(remove ? registry.m_stops.value(id).phase == PluginRegistry::StopPhase::Cleaning
                   : !registry.m_stops.contains(id),
            "a successful retry reaches cleanup only after checkpoint and shutdown success");
    host->setCallerClosing(&module, false);
    applicationRegistry->m_module_owner.remove(&module);
    applicationRegistry->m_records.remove(id);
}
} // namespace

RegistryJobLifetimeProbe::RegistryJobLifetimeProbe(PluginRegistry *registry)
    : m_registry(registry) {
    connect(registry, &PluginRegistry::enabledPluginsChanged, this,
            &RegistryJobLifetimeProbe::onRegistryChanged, qontrol::UNIQUE);
}

void RegistryJobLifetimeProbe::arm() {
    m_armed = true;
}

void RegistryJobLifetimeProbe::onRegistryChanged() {
    if (!m_armed || m_observed || m_job != nullptr || m_registry == nullptr) {
        return;
    }
    auto *job = m_registry->m_state_job.data();
    if (job == nullptr || job->action() != PluginStateAction::Enable) {
        return;
    }
    m_job = job;
    connect(job, &QThread::finished, this, &RegistryJobLifetimeProbe::onWorkerFinished,
            qontrol::UNIQUE);
    connect(job, &QThread::finished, job, &QObject::deleteLater, qontrol::UNIQUE);
}

void RegistryJobLifetimeProbe::onWorkerFinished() {
    if (m_observed) {
        return;
    }
    require(m_registry != nullptr,
            "native activation reaches its nested Host wait with a live registry");
    require(m_registry->m_state_consuming == PluginStateAction::Enable,
            "the finished worker callback runs inside native activation's nested loop");
    if (m_job != nullptr) {
        require(m_job->isFinished(), "the state worker finished before native activation");
        QCoreApplication::sendPostedEvents(m_job.data(), QEvent::MetaCall);
        if (m_job != nullptr) {
            m_job->deleteLater();
            QCoreApplication::sendPostedEvents(m_job.data(), QEvent::DeferredDelete);
        }
    }
    require(m_job == nullptr && m_registry->m_state_job == nullptr,
            "the finished state job is destroyed inside native activation's grant loop");
    m_observed = true;
    m_registry->rescanDiscoveredPlugins();
    require(m_registry->m_scan_requested && m_registry->m_state_job == nullptr,
            "a reentrant rescan stays queued until result consumption finishes");
}

void registryCheckpointFailureSmoke() {
    for (bool remove : {false, true}) {
        for (bool replyBeforeCheckpoint : {false, true}) {
            for (bool refuse : {false, true}) {
                checkpointFailure(remove, replyBeforeCheckpoint, refuse);
            }
        }
    }
}

void registryEnableRollbackFailureSmoke() {
    for (bool nativeFailure : {false, true}) {
        RegistryErrorFixture fixture;
        auto &registry = fixture.m_registry;
        auto id = fixture.m_id;
        PluginStateJob enable(PluginStateAction::Enable, id, 41, 0, false);
        enable.m_enable_persisted = true;
        if (nativeFailure) {
            enable.m_library.is_ok = true;
            enable.m_library.library_name = rust::String("registry-errors-invalid.so");
            enable.m_library.library.push_back(0);
        } else {
            enable.m_error = QStringLiteral("fixture library read failure");
        }
        registry.m_enabling.insert(id, enable.request());
        registry.finishPluginEnable(enable);
        require(!registry.pluginLoadError(id).isEmpty(), "failed enable retains its load error");
        require(registry.m_records.value(id).plugin == nullptr,
                "rollback supports an absent native plugin");
        require(registry.m_state_requests.size() == 1 &&
                    registry.m_state_requests.first().action == PluginStateAction::Disable &&
                    registry.m_state_requests.first().request != 0,
                "rollback is a correlated non-destructive Disable request");
        registry.onPluginStopPrepared(id, QStringLiteral("fixture rollback failure"));
        registry.advancePluginStops();
        require(registry.pluginStopError(id).contains(QStringLiteral("fixture rollback failure")),
                "failed rollback remains visible in the stop error");
        require(registry.pluginDisableRetryable(id),
                "failed rollback offers Retry disable without Remove");
        require(registry.setEnabled(id, false),
                "retry Disable is admitted without a loaded plugin");
        registry.onPluginStopPrepared(id, {});
        registry.advancePluginStops();
        require(!registry.pluginStopping(id), "successful Disable retry clears the failure state");
        require(registry.m_records.contains(id), "Disable retry preserves the installed record");
        require(!registry.pluginLoadError(id).isEmpty(),
                "Disable retry preserves the original load diagnostic");
    }
}

void registryEnableRollbackReadErrorSmoke() {
    RegistryErrorFixture fixture;
    auto &registry = fixture.m_registry;
    auto id = fixture.m_id;
    auto readError = QStringLiteral("fixture queued scan timed out");
    registry.m_removal_read_error = readError;
    require(!registry.setEnabled(id, false), "the scan error gates ordinary Disable admission");
    registry.removePlugin(id);
    require(registry.m_stops.isEmpty() && registry.m_state_requests.isEmpty(),
            "the scan error gates ordinary Remove admission without queuing cleanup");
    PluginStateJob enable(PluginStateAction::Enable, id, 41, 0, false);
    enable.m_enable_persisted = true;
    enable.m_library.is_ok = true;
    registry.m_enabling.insert(id, enable.request());
    registry.finishPluginEnable(enable);
    require(!registry.isPluginEnabled(id) && !registry.pluginLoadError(id).isEmpty(),
            "the failed scan blocks activation of the persisted enable");
    require(registry.pluginRemovalReadError() == readError,
            "compensation does not clear the scan admission error");
    require(registry.m_state_requests.size() == 1 &&
                registry.m_state_requests.first().action == PluginStateAction::Disable &&
                registry.m_state_requests.first().request != 0 && registry.m_stops.contains(id) &&
                !registry.m_stops.value(id).remove,
            "compensation queues a tracked non-destructive Disable despite the scan error");
    auto rollbackRequest = registry.m_stops.value(id).req_id;
    registry.onPluginStopPrepared(id, QStringLiteral("fixture rollback checkpoint failed"));
    registry.advancePluginStops();
    require(registry.pluginDisableRetryable(id) &&
                registry.pluginStopError(id).contains(
                    QStringLiteral("fixture rollback checkpoint failed")),
            "failed compensation preserves the rollback error and Retry disable state");
    require(!registry.setEnabled(id, false), "the scan error still gates user-requested retry");
    require(registry.m_stops.value(id).req_id == rollbackRequest,
            "refused user retry does not replace the failed compensation");
    PluginStateJob recovered(PluginStateAction::Scan, {}, 0, registry.m_state_revision, false);
    recovered.m_state.is_ok = true;
    PluginMetadata metadata{};
    metadata.id = rust::String(id.toStdString());
    recovered.m_metadata.push_back(metadata);
    registry.finishPluginScan(recovered);
    require(registry.pluginRemovalReadError().isEmpty() && registry.pluginDisableRetryable(id) &&
                registry.pluginStopError(id).contains(
                    QStringLiteral("fixture rollback checkpoint failed")),
            "a successful checked scan restores admission without losing the failed rollback");
    require(registry.setEnabled(id, false), "Retry disable is admitted after scan recovery");
    require(registry.m_stops.value(id).req_id != rollbackRequest,
            "the admitted retry has a fresh checkpoint request");
    registry.onPluginStopPrepared(id, {});
    registry.advancePluginStops();
    require(!registry.pluginStopping(id) && registry.m_records.contains(id) &&
                !registry.m_records.value(id).removal_phase.has_value(),
            "successful compensation retry retains the installation without starting removal");

    for (auto phase : {PluginRemovalPhase::Stopping, PluginRemovalPhase::PrivateData,
                       PluginRemovalPhase::Bundle, PluginRemovalPhase::Bookkeeping}) {
        RegistryErrorFixture journalFixture;
        auto &journalRegistry = journalFixture.m_registry;
        journalRegistry.m_removal_read_error = readError;
        journalRegistry.m_records[id].removal_phase = phase;
        journalRegistry.m_enabling.insert(id, enable.request());
        journalRegistry.finishPluginEnable(enable);
        require(journalRegistry.m_records.value(id).removal_phase == phase &&
                    journalRegistry.m_state_requests.isEmpty() &&
                    !journalRegistry.m_stops.contains(id),
                "compensation cannot replace a removal journal with ordinary Disable");
    }
}

void registryStaleScanErrorsSmoke() {
    RegistryErrorFixture fixture;
    auto &registry = fixture.m_registry;
    auto id = fixture.m_id;
    registry.m_records[id].load_error = QStringLiteral("obsolete native error");
    registry.m_state_revision = 8;
    PluginStateJob stale(PluginStateAction::Scan, {}, 0, 7, true);
    registry.finishPluginScan(stale);
    require(registry.m_scan_requested && registry.m_clear_load_errors,
            "a stale corrected-import scan requeues its clear-errors intent");
    PluginStateJob current(PluginStateAction::Scan, {}, 0, 8, registry.m_clear_load_errors);
    current.m_state.is_ok = true;
    PluginMetadata corrected{};
    corrected.id = rust::String(id.toStdString());
    current.m_metadata.push_back(corrected);
    registry.finishPluginScan(current);
    auto record = registry.m_records.constFind(id);
    require(record != registry.m_records.constEnd() && record->id == id && record->discovered,
            "the replacement scan retains the corrected installed record as discovered");
    require(registry.pluginLoadError(id).isEmpty(),
            "the replacement scan clears the obsolete load error");
    registry.m_clear_load_errors = true;
    PluginStateJob staleReload(PluginStateAction::Scan, {}, 0, 7, false);
    registry.finishPluginScan(staleReload);
    require(registry.m_clear_load_errors,
            "a stale ordinary reload cannot erase newer clear-errors intent");
}

void registryAccountDeletionRefreshSmoke() {
    for (bool succeeded : {false, true}) {
        RegistryListingModule module;
        RegistryErrorFixture fixture;
        auto &registry = fixture.m_registry;
        auto id = fixture.m_id;
        auto removed = QStringLiteral("deleted-account");
        auto retained = QStringLiteral("retained-account");
        QList<QPair<QString, QString>> before{{removed, removed}, {retained, retained}};
        registry.m_records[id].enabled = true;
        registry.m_records[id].modules = {&module};
        registry.m_module_owner.insert(&module,
                                       {.plugin_id = id,
                                        .module_id = QStringLiteral("registry.listing.module"),
                                        .plugin_hash = {},
                                        .external = true});
        registry.m_launcher_instances.insert(&module, before);
        QObject::connect(&module, &IModule::instances, &registry,
                         &PluginRegistry::onModuleInstances, qontrol::UNIQUE);
        QObject::connect(&module, &IModule::accountDeleted, &registry,
                         &PluginRegistry::onAccountDeleted,
                         static_cast<Qt::ConnectionType>(qontrol::UNIQUE | Qt::QueuedConnection));
        registry.requestModuleInstances(&module);
        require(registry.m_pending_list_requests.value(&module).contains(1),
                "a pre-deletion account snapshot is pending");
        registry.m_pending_deletions[&module].insert(91, removed);
        emit module.accountDeleted(91, removed,
                                   succeeded ? QString() : QStringLiteral("delete failed"));
        deliverShutdownReplies();
        if (!succeeded) {
            require(registry.launcherInstancesForModule(&module) == before &&
                        registry.m_pending_list_requests.value(&module) == QSet<ReqId>{1},
                    "failed deletion preserves cached accounts and accepted discovery");
            continue;
        }
        QList<QPair<QString, QString>> after{{retained, retained}};
        require(registry.launcherInstancesForModule(&module) == after &&
                    registry.m_pending_list_requests.value(&module) == QSet<ReqId>{2},
                "successful deletion publishes absence immediately and requests a fresh read");
        emit module.instances(1, before);
        require(registry.launcherInstancesForModule(&module) == after,
                "a snapshot started before deletion cannot resurrect the deleted row");
        after.first().second = QStringLiteral("fresh retained name");
        emit module.instances(2, after);
        require(
            registry.launcherInstancesForModule(&module) == after &&
                !registry.m_pending_list_requests.contains(&module),
            "the post-deletion snapshot publishes the retained account and retires its request");
    }
}
} // namespace smoke
