#include "PluginRegistry.h"
#include "Host.h"
#include "PluginLibrary.h"
#include "i18n/I18nManager.h"
#include "i18n/Tr.h"
#include "interfaces/instance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <common.h>
#include <silent.h>

namespace {
constexpr int PLUGIN_STOP_POLL_INTERVAL_MS = 20;
constexpr int PLUGIN_STATE_READ_DEADLINE_MS = 15000;

class MetadataOnlyPlugin final : public IPlugin {
public:
    explicit MetadataOnlyPlugin(QMap<QString, QString> meta) : m_meta(std::move(meta)) {
    }

    [[nodiscard]] QMap<QString, QString> meta() const override {
        return m_meta;
    }

    [[nodiscard]] QList<IModule *> modules() const override {
        return {};
    }

private:
    QMap<QString, QString> m_meta;
};

QMap<QString, QString> sidecarMeta(const PluginMetadata &bundle) {
    QMap<QString, QString> meta{
        {QStringLiteral("id"), QString::fromStdString(std::string(bundle.id))}};
    for (const auto &field : bundle.fields) {
        auto key = QString::fromStdString(std::string(field.key));
        switch (field.kind) {
        case PluginMetadataKind::Text:
            meta.insert(key, QString::fromStdString(std::string(field.text)));
            break;
        case PluginMetadataKind::Number:
            meta.insert(key, QString::number(field.number));
            break;
        case PluginMetadataKind::Flag:
            meta.insert(key, field.flag ? QStringLiteral("true") : QStringLiteral("false"));
            break;
        case PluginMetadataKind::Strings: {
            QStringList values;
            for (const auto &entry : field.items) {
                values.append(QString::fromStdString(std::string(entry)));
            }
            meta.insert(key, values.join(','));
            break;
        }
        }
    }

    return meta;
}

} // namespace

PluginRegistry::PluginRegistry(QObject *parent) : QObject(parent) {
    m_state_deadline.setSingleShot(true);
    m_state_deadline.setInterval(PLUGIN_STATE_READ_DEADLINE_MS);
    connect(&m_state_deadline, &QTimer::timeout, this, &PluginRegistry::onPluginStateReadDeadline,
            qontrol::UNIQUE);
    connect(&m_stop_timer, &QTimer::timeout, this, &PluginRegistry::advancePluginStops,
            qontrol::UNIQUE);
    m_stop_timer.setInterval(PLUGIN_STOP_POLL_INTERVAL_MS);
    auto *host = Host::get();
    if (host != nullptr && host->events() != nullptr) {
        connect(host->events(), &HostEvents::instanceRegistered, this,
                &PluginRegistry::onHostInstanceRegistered, qontrol::UNIQUE);
        connect(host->events(), &HostEvents::instanceRemoved, this,
                &PluginRegistry::onHostInstanceRemoved, qontrol::UNIQUE);
        connect(host->events(), &HostEvents::signerPollingChanged, this,
                &PluginRegistry::onSignerPollingChanged, qontrol::UNIQUE);
        connect(host->events(), &HostEvents::launcherRefreshRequested, this,
                &PluginRegistry::onLauncherRefreshRequested, qontrol::UNIQUE);
    }
    reloadEnabledPlugins();
}

PluginRegistry::~PluginRegistry() {
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if (!it->external) {
            continue;
        }
        // Active native work cannot be synchronously proved stopped here. Leave its
        // mapping alive until process exit instead of unmapping running code.
        if (it->plugin != nullptr && !it->modules.isEmpty()) {
            continue;
        }
        delete it->library;
        it->library = nullptr;
    }
    qDeleteAll(m_metadata_plugins);
    m_metadata_plugins.clear();
}

void PluginRegistry::rescanDiscoveredPlugins() {
    requestPluginState(true);
}

void PluginRegistry::requestPluginState(bool clear_errors) {
    ++m_state_revision;
    m_scan_requested = true;
    m_clear_load_errors |= clear_errors;
    if (!m_state_deadline.isActive()) {
        m_state_deadline.start();
    }
    startPluginStateJob();
    emit pluginStateChanged();
}

auto PluginRegistry::pluginStateLoading() const -> bool {
    return m_scan_requested || m_state_consuming == PluginStateAction::Scan ||
           (m_state_job != nullptr && m_state_job->action() == PluginStateAction::Scan);
}

void PluginRegistry::onPluginStateReadDeadline() {
    if (!pluginStateLoading()) {
        return;
    }
    ++m_state_revision;
    m_removal_read_error = QStringLiteral(
        "Reading plugin state is still waiting for storage. Known records were kept. "
        "Retry after storage becomes available.");
    emit pluginStateChanged();
}

auto PluginRegistry::pluginEnablePending(const QString &plugin_id) const -> bool {
    return m_enabling.contains(plugin_id);
}

auto PluginRegistry::pluginHasAccessHistory(const QString &plugin_id) const -> bool {
    return m_access_snapshot.contains(plugin_id);
}

void PluginRegistry::onPluginAccessSaved([[maybe_unused]] uint64_t revision,
                                         [[maybe_unused]] const QString &error) {
    // A save error can follow rename, so reread even when publication reports failure.
    requestPluginState(false);
}

void PluginRegistry::startPluginStateJob() {
    if (m_state_job != nullptr || m_state_consuming.has_value()) {
        return;
    }
    StateRequest request;
    if (!m_state_requests.isEmpty()) {
        request = m_state_requests.takeFirst();
    } else if (m_scan_requested) {
        request = {.action = PluginStateAction::Scan,
                   .plugin_id = {},
                   .request = 0,
                   .revision = m_state_revision,
                   .clear_errors = m_clear_load_errors};
        m_scan_requested = false;
        m_clear_load_errors = false;
    } else {
        return;
    }
    m_state_job = new PluginStateJob(request.action, request.plugin_id, request.request,
                                     request.revision, request.clear_errors);
    m_state_job->setReceiver(this);
    connect(m_state_job, &QThread::finished, this, &PluginRegistry::onPluginStateJobFinished,
            qontrol::UNIQUE);
    m_state_job->start();
}

void PluginRegistry::onPluginStateJobFinished() {
    auto job = m_state_job;
    if (job == nullptr || sender() != job) {
        return;
    }
    QPointer<PluginRegistry> alive(this);
    m_state_consuming = job->action();
    switch (job->action()) {
    case PluginStateAction::Scan:
        finishPluginScan(*job);
        break;
    case PluginStateAction::Enable:
        finishPluginEnable(*job);
        break;
    case PluginStateAction::Disable:
        if (m_stops.contains(job->pluginId()) &&
            m_stops.value(job->pluginId()).req_id == job->request()) {
            onPluginStopPrepared(job->pluginId(), job->error());
        }
        break;
    case PluginStateAction::BeginRemoval:
        if (m_stops.contains(job->pluginId()) &&
            m_stops.value(job->pluginId()).req_id == job->request()) {
            onPluginStopPrepared(job->pluginId(), job->error());
        }
        break;
    case PluginStateAction::RemoveData:
        if (m_stops.contains(job->pluginId()) &&
            m_stops.value(job->pluginId()).req_id == job->request()) {
            onPluginDataRemoved(job->pluginId(), job->error());
        }
        break;
    }
    if (alive == nullptr) {
        return;
    }
    m_state_job = nullptr;
    m_state_consuming.reset();
    if (job != nullptr) {
        job->deleteLater();
    }
    startPluginStateJob();
    emit pluginStateChanged();
}

void PluginRegistry::finishPluginScan(const PluginStateJob &job) {
    if (job.revision() == m_state_revision) {
        applyPluginState(job);
    } else {
        m_scan_requested = true;
        m_clear_load_errors |= job.clearErrors();
    }
}

void PluginRegistry::applyPluginState(const PluginStateJob &job) {
    auto removals = job.state();
    auto metadata = job.metadata();
    auto clearErrors = job.clearErrors();
    QPointer<PluginRegistry> alive(this);
    m_state_deadline.stop();
    if (!removals.is_ok) {
        m_removal_read_error = QString::fromStdString(std::string(removals.error));
        emit enabledPluginsChanged();
        return;
    }
    m_state_loaded = true;
    m_removal_read_error.clear();
    m_enabled_snapshot.clear();
    for (const auto &id : removals.enabled_plugins) {
        m_enabled_snapshot.insert(QString::fromStdString(std::string(id)));
    }
    m_access_snapshot.clear();
    for (const auto &entry : removals.access) {
        m_access_snapshot.insert(QString::fromStdString(std::string(entry.plugin_id)));
    }
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if (!it->external) {
            continue;
        }
        it->discovered = false;
        it->removal_phase.reset();
        // A scan gives a plugin that failed to load another chance.
        if (clearErrors && it->plugin == nullptr) {
            it->load_error.clear();
        }
    }

    // Only the plugins sealed in the app state: nothing found loose in
    // ~/.silent/plugins/ is ever read (ws-plaude/silent#41).
    for (const auto &bundle : metadata) {
        auto meta = sidecarMeta(bundle);
        QString pluginId = meta.value(QStringLiteral("id")).trimmed();
        if (pluginId.isEmpty()) {
            continue;
        }

        auto &record = m_records[pluginId];
        record.id = pluginId;
        record.external = true;
        record.name = meta.value(QStringLiteral("name")).trimmed();
        record.version = meta.value(QStringLiteral("version")).trimmed();
        record.meta = meta;
        record.discovered = true;

        if (record.plugin == nullptr) {
            record.plugin_object = nullptr;
            record.modules.clear();
        }
    }

    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        if (!it->external) {
            continue;
        }
        if (!it->discovered && it->plugin != nullptr) {
            qWarning() << "Plugin no longer discoverable on disk; keeping runtime state until "
                          "explicit disable:"
                       << it.key();
        }
    }

    for (const auto &entry : removals.entries) {
        auto pluginId = QString::fromStdString(std::string(entry.plugin_id));
        auto &record = m_records[pluginId];
        record.id = pluginId;
        record.external = true;
        record.discovered = true;
        record.enabled = false;
        record.removal_phase = entry.phase;
        record.meta.insert(QStringLiteral("id"), pluginId);
    }
    for (const auto &id : m_stops.keys()) {
        if (m_records.contains(id)) {
            m_records[id].discovered = true;
        }
    }

    rebuildPluginList();
    if (m_reload_requested) {
        m_reload_requested = false;
        applyEnabledPlugins();
    }
    if (alive == nullptr) {
        return;
    }
    emit enabledPluginsChanged();
}

void PluginRegistry::reloadEnabledPlugins() {
    m_reload_requested = true;
    requestPluginState(false);
}

void PluginRegistry::applyEnabledPlugins() {
    if (!m_removal_read_error.isEmpty()) {
        return;
    }
    QHash<QString, bool> desiredEnabled;
    for (const auto &pluginId : m_enabled_snapshot) {
        desiredEnabled.insert(pluginId, true);
    }

    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        if (it->builtin) {
            desiredEnabled.insert(it.key(), true);
        } else {
            desiredEnabled.insert(it.key(), desiredEnabled.value(it.key(), false));
        }
    }

    auto ids = m_records.keys();
    QPointer<PluginRegistry> alive(this);
    for (const auto &id : ids) {
        if (!m_records.value(id).external) {
            continue;
        }
        if (pluginStopping(id)) {
            continue;
        }
        if (desiredEnabled.value(id, false)) {
            if (!m_records.value(id).enabled && !m_enabling.contains(id)) {
                setEnabled(id, true);
                if (alive == nullptr) {
                    return;
                }
            }
            continue;
        }
        if (m_records.value(id).enabled) {
            disablePlugin(id);
            if (alive == nullptr) {
                return;
            }
        }
    }
}

void PluginRegistry::registerBuiltin(IPlugin *plugin) {
    if (plugin == nullptr) {
        return;
    }
    QString id = pluginId(plugin);
    auto existingIt = m_records.find(id);
    if (existingIt != m_records.end() && existingIt->plugin == plugin) {
        return;
    }

    auto &record = m_records[id];
    record.id = id;
    record.builtin = true;
    record.external = false;
    record.plugin = plugin;
    record.enabled = true;
    if (!m_plugin_order.contains(id)) {
        m_plugin_order.append(id);
    }
    syncPluginTranslations(id, true);

    QList<IModule *> modules = plugin->modules();
    std::ranges::remove(modules, nullptr);
    record.modules = modules;
    for (auto *module : modules) {
        m_builtin_modules.insert(module);
        activateModule(module, id);
    }

    rebuildPluginList();
}

QList<IPlugin *> PluginRegistry::plugins() const {
    return m_listed_plugins;
}

QList<IModule *> PluginRegistry::enabledModulesForLauncher() const {
    QList<IModule *> enabled;
    for (const auto &id : m_plugin_order) {
        auto recordIt = m_records.find(id);
        if (recordIt == m_records.end() || recordIt->plugin == nullptr) {
            continue;
        }
        if (!isPluginEnabled(id)) {
            continue;
        }
        for (auto *module : recordIt->modules) {
            if (module != nullptr) {
                enabled.append(module);
            }
        }
    }
    return enabled;
}

QList<QPair<QString, QString>>
PluginRegistry::launcherInstancesForModule(const IModule *module) const {
    auto *nonConstModule = const_cast<IModule *>(module);
    return m_launcher_instances.value(nonConstModule);
}

QString PluginRegistry::instanceOwnerPluginId(const QString &id) const {
    return m_instance_owner_by_id.value(id.trimmed());
}

bool PluginRegistry::isCreatorInstanceId(const QString &id) {
    QString trimmed = id.trimmed();
    return trimmed == QStringLiteral("create") || trimmed.endsWith(QStringLiteral(".create")) ||
           trimmed.endsWith(QStringLiteral(".restore"));
}

bool PluginRegistry::isBuiltinModule(IModule *module) const {
    return m_builtin_modules.contains(module);
}

auto PluginRegistry::pluginHash(const QString &plugin_id) const -> QString {
    auto it = m_records.constFind(plugin_id);
    if (it == m_records.constEnd() || it->builtin) {
        return {};
    }
    return it->plugin_hash;
}

auto PluginRegistry::pluginVersion(const QString &plugin_id) const -> QString {
    auto it = m_records.constFind(plugin_id);
    return it == m_records.constEnd() ? QString() : it->version;
}

auto PluginRegistry::caller(IModule *module) const -> std::optional<plugin::Caller> {
    if (module == nullptr) {
        return std::nullopt;
    }
    auto ownerIt = m_module_owner.constFind(module);
    if (ownerIt == m_module_owner.constEnd()) {
        return std::nullopt;
    }
    if (ownerIt->external && !m_removal_read_error.isEmpty()) {
        return std::nullopt;
    }
    if (m_stops.contains(ownerIt->plugin_id)) {
        return std::nullopt;
    }
    return plugin::Caller{
        .moduleId = ownerIt->module_id,
        .pluginId = ownerIt->plugin_id,
        .external = ownerIt->external,
        .pluginHash = ownerIt->plugin_hash,
    };
}

QList<PluginRegistry::CreatorLauncher> PluginRegistry::externalCreatorLaunchers() const {
    QList<CreatorLauncher> creators;
    for (auto *module : enabledModulesForLauncher()) {
        if (module == nullptr || isBuiltinModule(module)) {
            continue;
        }
        for (const auto &instance : launcherInstancesForModule(module)) {
            if (isCreatorInstanceId(instance.first)) {
                creators.append({.module = module,
                                 .id = instance.first.trimmed(),
                                 .name = instance.second.trimmed()});
            }
        }
    }
    return creators;
}

bool PluginRegistry::startLauncherInstance(IModule *module, const QString &instance_id) {
    return startOwnedInstance(module, instance_id);
}

void PluginRegistry::deleteInstance(IModule *module, const QString &instance_id) {
    auto known = m_launcher_instances.value(module);
    auto owns = std::ranges::any_of(
        known, [&instance_id](const auto &item) { return item.first == instance_id; });
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    if (host != nullptr && host->instanceOwner(instance_id) != nullptr &&
        host->instanceOwner(instance_id) != module) {
        owns = false;
    }
    if (!caller(module).has_value() || !owns || isCreatorInstanceId(instance_id) ||
        instance_id.isEmpty()) {
        emit accountDeletionFinished(module, instance_id,
                                     QStringLiteral("Unknown module/account identity"));
        return;
    }
    if (m_pending_deletions.value(module).values().contains(instance_id)) {
        emit accountDeletionFinished(module, instance_id,
                                     QStringLiteral("Account deletion is already pending"));
        return;
    }
    if (++m_delete_req_id == 0) {
        ++m_delete_req_id;
    }
    m_pending_deletions[module].insert(m_delete_req_id, instance_id);
    module->deleteAccount(m_delete_req_id, instance_id);
}

void PluginRegistry::onAccountDeleted(ReqId req_id, const QString &id, const QString &error) {
    auto *module = qobject_cast<IModule *>(sender());
    if (!m_pending_deletions.contains(module) ||
        !m_pending_deletions.value(module).contains(req_id) ||
        m_pending_deletions.value(module).value(req_id) != id) {
        return;
    }
    m_pending_deletions[module].remove(req_id);
    if (m_pending_deletions.value(module).isEmpty()) {
        m_pending_deletions.remove(module);
    }
    if (error.isEmpty()) {
        auto instances = m_launcher_instances.find(module);
        if (instances != m_launcher_instances.end()) {
            instances->removeIf([&id](const auto &entry) { return entry.first == id; });
        }
        m_pending_list_requests.remove(module);
        m_sync_list_responses.remove(module);
        if (auto *host = dynamic_cast<HostImpl *>(Host::get());
            host != nullptr && host->instanceOwner(id) == module) {
            host->removeInstance(id);
        }
        if (caller(module).has_value()) {
            requestModuleInstances(module);
        }
    }
    emit accountDeletionFinished(module, id, error);
}

auto PluginRegistry::accountDeletionPending(const QString &id) const -> bool {
    return std::ranges::any_of(m_pending_deletions, [&id](const auto &requests) {
        return requests.values().contains(id);
    });
}

bool PluginRegistry::isPluginEnabled(const QString &plugin_id) const {
    auto recordIt = m_records.find(plugin_id);
    if (recordIt == m_records.end()) {
        return false;
    }
    return recordIt->enabled;
}

bool PluginRegistry::isPluginBuiltin(const QString &plugin_id) const {
    auto recordIt = m_records.find(plugin_id);
    if (recordIt == m_records.end()) {
        return false;
    }
    return recordIt->builtin;
}

bool PluginRegistry::pluginHasBuiltinModules(const QString &plugin_id) const {
    auto *plugin = pluginById(plugin_id);
    if (plugin == nullptr) {
        return false;
    }

    auto recordIt = m_records.find(plugin_id);
    if (recordIt == m_records.end()) {
        return false;
    }
    auto modules = recordIt->modules;
    return std::ranges::any_of(modules,
                               [this](auto *module) { return m_builtin_modules.contains(module); });
}

auto PluginRegistry::setEnabled(const QString &plugin_id, bool enabled) -> bool {
    if (plugin_id.trimmed().isEmpty() || !m_removal_read_error.isEmpty()) {
        return false;
    }
    if ((m_stops.contains(plugin_id) && (enabled || !pluginDisableRetryable(plugin_id))) ||
        (enabled && m_records.value(plugin_id).removal_phase.has_value())) {
        return false;
    }
    if (!enabled) {
        beginPluginStop(plugin_id, false);
        return m_stops.contains(plugin_id);
    }

    if (m_enabling.contains(plugin_id)) {
        return true;
    }

    if (!m_state_loaded) {
        return false;
    }

    bool previous = isPluginEnabled(plugin_id);
    if (previous == enabled) {
        return true;
    }

    auto record = m_records.find(plugin_id);
    if (record == m_records.end() || (!record->discovered && !record->builtin)) {
        return false;
    }
    auto request = ++m_delete_req_id;
    if (request == 0) {
        request = ++m_delete_req_id;
    }
    ++m_state_revision;
    m_enabling.insert(plugin_id, request);
    m_state_requests.append({.action = PluginStateAction::Enable,
                             .plugin_id = plugin_id,
                             .request = request,
                             .revision = m_state_revision});
    startPluginStateJob();
    emit enabledPluginsChanged();
    return true;
}

void PluginRegistry::finishPluginEnable(const PluginStateJob &job) {
    auto id = job.pluginId();
    if (m_enabling.value(id, 0) != job.request() || m_stops.contains(id) ||
        !m_records.contains(id)) {
        m_enabling.remove(id);
        emit pluginEnableFinished(id,
                                  QStringLiteral("Enable was superseded by shutdown or removal"));
        return;
    }
    auto error = job.error();
    // Activation can enter a nested event loop that deletes the finished job.
    auto enablePersisted = job.enablePersisted();
    auto library = job.library();
    QPointer<PluginRegistry> alive(this);
    if (error.isEmpty()) {
        m_records[id].enabled = true;
        auto enabled = enablePlugin(id, library);
        if (alive == nullptr) {
            return;
        }
        if (!enabled) {
            m_records[id].enabled = false;
            error = m_records.value(id).load_error;
            if (error.isEmpty()) {
                error = QStringLiteral("Plugin activation failed");
                m_records[id].load_error = error;
            }
        }
    } else {
        m_records[id].enabled = false;
        m_records[id].load_error = error;
    }
    if (!m_stops.contains(id)) {
        syncPluginTranslations(id, error.isEmpty());
    }
    if (alive == nullptr) {
        return;
    }
    m_enabling.remove(id);
    if (!error.isEmpty() && enablePersisted) {
        // Compensate an accepted enable even when a later scan has failed admission.
        startPluginStop(id, false);
    }
    if (alive == nullptr) {
        return;
    }
    emit enabledPluginsChanged();
    if (alive == nullptr) {
        return;
    }
    emit pluginEnableFinished(id, error);
}

auto PluginRegistry::pluginName(const QString &plugin_id) const -> QString {
    auto recordIt = m_records.find(plugin_id);
    if (recordIt == m_records.end()) {
        return plugin_id;
    }
    if (!recordIt->name.isEmpty()) {
        return recordIt->name;
    }
    if (recordIt->plugin != nullptr) {
        QString name = recordIt->plugin->meta().value(QStringLiteral("name")).trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return plugin_id;
}

auto PluginRegistry::pluginDescription(const QString &plugin_id) const -> QString {
    auto recordIt = m_records.find(plugin_id);
    if (recordIt == m_records.end()) {
        return {};
    }
    // The sidecar describes an external plugin; a builtin describes itself.
    QString description = recordIt->meta.value(QStringLiteral("description")).trimmed();
    if (description.isEmpty() && recordIt->plugin != nullptr) {
        description = recordIt->plugin->meta().value(QStringLiteral("description")).trimmed();
    }
    return description;
}

auto PluginRegistry::pluginLoadError(const QString &plugin_id) const -> QString {
    return m_records.value(plugin_id).load_error;
}

auto PluginRegistry::pluginRemovalReadError() const -> QString {
    return m_removal_read_error;
}

auto PluginRegistry::pluginMetadata(const QString &plugin_id) const -> QMap<QString, QString> {
    return m_records.value(plugin_id).meta;
}

auto PluginRegistry::pluginAccountNames(const QString &plugin_id) const -> QStringList {
    QStringList names;
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    auto recordIt = m_records.find(plugin_id);
    if (host == nullptr || recordIt == m_records.end()) {
        return names;
    }
    for (auto *instance : host->liveInstances()) {
        if (instance == nullptr || m_instance_owner_by_id.value(instance->id()) != plugin_id) {
            continue;
        }
        auto *account = instance->account();
        if (account == nullptr || !account->implemented()) {
            continue;
        }
        names.append(instanceName(recordIt->modules, instance->id()));
    }
    return names;
}

auto PluginRegistry::instanceName(const QList<IModule *> &modules, const QString &instance_id) const
    -> QString {
    // The name the module gave when it listed its instances.
    for (auto *module : modules) {
        for (const auto &instance : m_launcher_instances.value(module)) {
            if (instance.first == instance_id && !instance.second.trimmed().isEmpty()) {
                return instance.second.trimmed();
            }
        }
    }
    return instance_id;
}

void PluginRegistry::onModuleInstances(ReqId req_id, QList<QPair<QString, QString>> instances) {
    auto *module = qobject_cast<IModule *>(sender());
    if (module == nullptr) {
        return;
    }
    applyModuleInstances(module, req_id, std::move(instances));
}

void PluginRegistry::applyModuleInstances(IModule *module, ReqId req_id,
                                          QList<QPair<QString, QString>> instances) {
    auto pendingIt = m_pending_list_requests.find(module);
    if (pendingIt == m_pending_list_requests.end() || !pendingIt->remove(req_id)) {
        if (req_id != 0 && m_module_owner.contains(module)) {
            m_sync_list_responses.insert(module, {req_id, std::move(instances)});
        }
        return;
    }
    if (pendingIt->isEmpty()) {
        m_pending_list_requests.erase(pendingIt);
    }

    if (!m_module_owner.contains(module)) {
        return;
    }
    m_launcher_instances.insert(module, instances);

    // Consumers (account discovery, launcher list) read launcherInstancesForModule();
    // these results often arrive asynchronously, so notify them to refresh.
    emit enabledPluginsChanged();
}

void PluginRegistry::onModuleAutoStart(ReqId req_id, QStringList ids) {
    auto *module = qobject_cast<IModule *>(sender());
    if (module == nullptr) {
        return;
    }
    applyModuleAutoStart(module, req_id, std::move(ids));
}

void PluginRegistry::applyModuleAutoStart(IModule *module, ReqId req_id, QStringList ids) {
    auto pendingIt = m_pending_auto_start_requests.find(module);
    if (pendingIt == m_pending_auto_start_requests.end() || !pendingIt->remove(req_id)) {
        if (req_id != 0 && m_module_owner.contains(module)) {
            m_sync_auto_start_responses.insert(module, {req_id, std::move(ids)});
        }
        return;
    }
    if (pendingIt->isEmpty()) {
        m_pending_auto_start_requests.erase(pendingIt);
    }

    if (!m_module_owner.contains(module)) {
        return;
    }

    for (const auto &id : ids) {
        startOwnedInstance(module, id);
    }
}

void PluginRegistry::onModuleError(std::optional<ReqId> req_id, const QString &message) {
    qWarning() << "Module error:" << message;
    if (!req_id.has_value()) {
        return;
    }

    auto *module = qobject_cast<IModule *>(sender());
    if (module == nullptr) {
        return;
    }

    if (auto listIt = m_pending_list_requests.find(module);
        listIt != m_pending_list_requests.end()) {
        listIt->remove(req_id.value());
        if (listIt->isEmpty()) {
            m_pending_list_requests.erase(listIt);
        }
    }

    if (auto autoIt = m_pending_auto_start_requests.find(module);
        autoIt != m_pending_auto_start_requests.end()) {
        autoIt->remove(req_id.value());
        if (autoIt->isEmpty()) {
            m_pending_auto_start_requests.erase(autoIt);
        }
    }
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
QString PluginRegistry::pluginId(IPlugin *plugin) const {
    if (plugin == nullptr) {
        return QString();
    }
    auto meta = plugin->meta();
    QString id = meta.value(QStringLiteral("id")).trimmed();
    if (!id.isEmpty()) {
        return id;
    }
    return QStringLiteral("builtin.%1").arg(reinterpret_cast<quintptr>(plugin), 0, 16);
}

QString PluginRegistry::pluginI18nDir(IPlugin *plugin) const {
    if (plugin == nullptr) {
        return {};
    }

    auto meta = plugin->meta();
    QString explicitDir = meta.value(QStringLiteral("i18n_dir")).trimmed();
    if (!explicitDir.isEmpty()) {
        return explicitDir;
    }

    QString id = pluginId(plugin);
    if (id.isEmpty()) {
        return {};
    }
    auto recordIt = m_records.find(id);
    if (recordIt != m_records.end() && !recordIt->i18n_dir.isEmpty()) {
        return recordIt->i18n_dir;
    }
    return QDir::homePath() + QStringLiteral("/.silent/plugins/%1/i18n").arg(id);
}

void PluginRegistry::syncPluginTranslations(const QString &plugin_id, bool enabled) {
    auto *plugin = pluginById(plugin_id);
    auto *host = Host::get();
    if (plugin == nullptr || host == nullptr || host->i18n() == nullptr) {
        return;
    }

    if (!enabled) {
        host->i18n()->unregisterPluginTranslations(plugin_id);
        return;
    }

    // An external plugin's catalogs come from its bundle, never from disk.
    auto recordIt = m_records.find(plugin_id);
    if (recordIt != m_records.end() && recordIt->external) {
        if (recordIt->catalogs.isEmpty()) {
            host->i18n()->unregisterPluginTranslations(plugin_id);
        } else {
            i18n::I18nManager::get()->registerPluginCatalogs(plugin_id, recordIt->catalogs);
        }
        return;
    }

    QString dir = pluginI18nDir(plugin);
    if (dir.isEmpty() || !QFileInfo::exists(dir) || !QFileInfo(dir).isDir()) {
        host->i18n()->unregisterPluginTranslations(plugin_id);
        return;
    }
    host->i18n()->registerPluginTranslations(plugin_id, dir);
}

IPlugin *PluginRegistry::pluginById(const QString &plugin_id) const {
    if (plugin_id.trimmed().isEmpty()) {
        return nullptr;
    }

    auto recordIt = m_records.find(plugin_id);
    if (recordIt != m_records.end()) {
        return recordIt->plugin;
    }
    return nullptr;
}

bool PluginRegistry::enablePlugin(const QString &plugin_id, const PluginLibraryResult &opened) {
    if (!m_removal_read_error.isEmpty() || m_stops.contains(plugin_id) ||
        m_records.value(plugin_id).removal_phase.has_value()) {
        return false;
    }
    auto recordIt = m_records.find(plugin_id);
    if (recordIt != m_records.end() && recordIt->builtin) {
        syncPluginTranslations(plugin_id, true);
        return true;
    }

    if (recordIt == m_records.end()) {
        qWarning() << "Enable skipped: plugin not discovered" << plugin_id;
        return false;
    }

    auto &record = *recordIt;
    if (!record.discovered) {
        qWarning() << "Enable skipped: plugin is not currently discovered" << plugin_id;
        return false;
    }
    if (record.plugin != nullptr) {
        syncPluginTranslations(plugin_id, true);
        return true;
    }
    if (record.library != nullptr) {
        qWarning()
            << "Enable blocked: plugin still has active loader; disable/unload cleanup required"
            << plugin_id;
        return false;
    }

    // The bytes opened from the app state are the ones hashed and the ones loaded:
    // no file on disk between the two.
    if (!opened.is_ok) {
        qWarning() << "Enable failed: plugin unreadable" << plugin_id;
        record.load_error = QString::fromStdString(std::string(opened.error));
        return false;
    }
    QByteArray bytes(reinterpret_cast<const char *>(opened.library.data()),
                     static_cast<qsizetype>(opened.library.size()));
    record.plugin_hash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    record.catalogs.clear();
    for (const auto &catalog : opened.catalogs) {
        record.catalogs.insert(QString::fromStdString(std::string(catalog.name)),
                               QString::fromStdString(std::string(catalog.text)));
    }

    record.library = new PluginLibrary();
    if (!record.library->load(bytes, QString::fromStdString(std::string(opened.library_name)))) {
        qWarning() << "Plugin load failed:" << plugin_id << record.library->errorString();
        record.load_error = record.library->errorString();
        delete record.library;
        record.library = nullptr;
        return false;
    }
    QObject *root = record.library->instance();

    auto *plugin = qobject_cast<IPlugin *>(root);
    if (plugin == nullptr) {
        qWarning() << "Plugin interface cast failed:" << plugin_id;
        record.load_error = TR("settings-plugin-not-silent");
        delete record.library;
        record.library = nullptr;
        return false;
    }

    // The installed record names the plugin: a library claiming another id is refused
    // before module activation, so it never acts as that plugin through the host.
    if (QString claimedId = pluginId(plugin); claimedId != plugin_id) {
        qWarning() << "Plugin claims another id:" << plugin_id << claimedId;
        record.load_error = QStringLiteral("plugin library claims the id '%1'").arg(claimedId);
        delete record.library;
        record.library = nullptr;
        return false;
    }

    record.load_error.clear();
    record.plugin_object = root;
    record.plugin = plugin;
    record.modules = plugin->modules();
    std::ranges::remove(record.modules, nullptr);

    if (!m_plugin_order.contains(plugin_id)) {
        m_plugin_order.append(plugin_id);
    }
    auto modules = record.modules;
    QPointer<PluginRegistry> alive(this);
    for (auto *module : modules) {
        activateModule(module, plugin_id);
        if (alive == nullptr) {
            return false;
        }
    }
    rebuildPluginList();
    syncPluginTranslations(plugin_id, true);
    return alive != nullptr;
}

bool PluginRegistry::disablePlugin(const QString &plugin_id) {
    auto recordIt = m_records.find(plugin_id);
    if (recordIt != m_records.end() && recordIt->builtin) {
        return false;
    }

    if (recordIt == m_records.end()) {
        return false;
    }

    if (recordIt->plugin == nullptr && recordIt->library == nullptr) {
        return true;
    }
    beginPluginStop(plugin_id, false);
    return true;
}

auto PluginRegistry::pluginStopping(const QString &plugin_id) const -> bool {
    return m_stops.contains(plugin_id) || m_records.value(plugin_id).removal_phase.has_value();
}

auto PluginRegistry::pluginStopPending(const QString &plugin_id) const -> bool {
    auto stop = m_stops.constFind(plugin_id);
    return stop != m_stops.constEnd() &&
           (stop->phase != StopPhase::Failed || stop->checkpoint == StopCheckpoint::Pending ||
            !stop->pending.isEmpty());
}

auto PluginRegistry::pluginDisableRetryable(const QString &plugin_id) const -> bool {
    auto stop = m_stops.constFind(plugin_id);
    return stop != m_stops.constEnd() && stop->phase == StopPhase::Failed && !stop->remove &&
           stop->checkpoint != StopCheckpoint::Pending && stop->pending.isEmpty();
}

auto PluginRegistry::pluginStopError(const QString &plugin_id) const -> QString {
    auto stop = m_stops.constFind(plugin_id);
    if (stop != m_stops.constEnd()) {
        if (stop->error.isEmpty()) {
            return QString();
        }
        return stop->pending.isEmpty()
                   ? stop->error
                   : stop->error +
                         QStringLiteral("\nWaiting for remaining module shutdowns before retry.");
    }
    auto phase = m_records.value(plugin_id).removal_phase;
    if (!phase.has_value()) {
        return QString();
    }
    switch (*phase) {
    case PluginRemovalPhase::Stopping:
        return TR("settings-plugin-stop-interrupted");
    case PluginRemovalPhase::PrivateData:
        return TR("settings-plugin-removal-interrupted");
    case PluginRemovalPhase::Bundle:
        return TR("settings-plugin-bundle-pending");
    case PluginRemovalPhase::Bookkeeping:
        return TR("settings-plugin-bookkeeping-pending");
    }
    return QStringLiteral("Unknown removal phase; plugin remains unavailable");
}

void PluginRegistry::removePlugin(const QString &plugin_id) {
    beginPluginStop(plugin_id, true);
}

void PluginRegistry::beginPluginStop(const QString &plugin_id, bool remove) {
    if (!m_removal_read_error.isEmpty()) {
        emit pluginStopFinished(plugin_id, false, m_removal_read_error);
        return;
    }
    startPluginStop(plugin_id, remove);
}

void PluginRegistry::startPluginStop(const QString &plugin_id, bool remove) {
    QPointer<PluginRegistry> alive(this);
    auto record = m_records.constFind(plugin_id);
    if (record == m_records.constEnd() || !record->external || record->builtin) {
        emit pluginStopFinished(plugin_id, false,
                                QStringLiteral("Only external plugins can be removed or disabled"));
        return;
    }
    if (record->removal_phase.has_value() && !remove) {
        emit pluginStopFinished(
            plugin_id, false, QStringLiteral("Removal is pending; retry Remove to finish cleanup"));
        return;
    }
    if (pluginStopPending(plugin_id)) {
        emit pluginStopFinished(plugin_id, false,
                                QStringLiteral("Plugin shutdown is already pending"));
        return;
    }
    auto modules = record->modules;
    for (auto *module : modules) {
        if (m_pending_deletions.contains(module)) {
            emit pluginStopFinished(
                plugin_id, false,
                QStringLiteral("Wait for pending account deletion before retrying"));
            return;
        }
    }
    auto reqId = ++m_delete_req_id;
    if (reqId == 0) {
        reqId = ++m_delete_req_id;
    }
    m_stops.insert(plugin_id, {.remove = remove, .req_id = reqId});
    m_enabling.remove(plugin_id);
    ++m_state_revision;
    m_records[plugin_id].enabled = false;
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    for (auto *module : modules) {
        if (host != nullptr) {
            host->setCallerClosing(module, true);
            if (alive == nullptr) {
                return;
            }
        }
    }
    emit enabledPluginsChanged();
    if (alive == nullptr) {
        return;
    }
    m_state_requests.append(
        {.action = remove ? PluginStateAction::BeginRemoval : PluginStateAction::Disable,
         .plugin_id = plugin_id,
         .request = reqId,
         .revision = m_state_revision});
    startPluginStateJob();
    m_stop_timer.start();
}

void PluginRegistry::onPluginStopPrepared(const QString &plugin_id, const QString &error) {
    if (!m_stops.contains(plugin_id)) {
        return;
    }
    auto &stop = m_stops[plugin_id];
    stop.checkpoint = error.isEmpty() ? StopCheckpoint::Saved : StopCheckpoint::Failed;
    if (!error.isEmpty()) {
        m_records[plugin_id].enabled = false;
        auto failure =
            QStringLiteral(
                "Could not persist disabled/removal state; library and data retained. Plugin is "
                "unavailable in this session; retry. Restart state is not guaranteed: %1")
                .arg(error);
        stop.error = stop.error.isEmpty() ? failure : failure + QLatin1Char('\n') + stop.error;
        failure = stop.error;
        emit enabledPluginsChanged();
        emit pluginStopFinished(plugin_id, false, failure);
        return;
    }
    auto &record = m_records[plugin_id];
    record.enabled = false;
    if (stop.remove && !record.removal_phase.has_value()) {
        record.removal_phase = PluginRemovalPhase::Stopping;
    }
    emit enabledPluginsChanged();
}

void PluginRegistry::requestModuleShutdown(const QString &plugin_id) {
    auto modules = m_records.value(plugin_id).modules;
    auto request = m_stops.value(plugin_id).req_id;
    for (auto *module : modules) {
        m_stops[plugin_id].pending.insert(module);
        connect(module, &IModule::shutdownFinished, this, &PluginRegistry::onModuleShutdownFinished,
                static_cast<Qt::ConnectionType>(qontrol::UNIQUE | Qt::QueuedConnection));
    }
    stopPluginInstances(plugin_id);
    m_stops[plugin_id].phase = StopPhase::Stopping;
    for (auto *module : modules) {
        auto *host = dynamic_cast<HostImpl *>(Host::get());
        if (host != nullptr) {
            host->enterFrame(module);
        }
        module->setSignerPolling(false);
        module->shutdown(request);
        if (host != nullptr) {
            host->leaveFrame(module);
        }
    }
}

void PluginRegistry::onModuleShutdownFinished(ReqId req_id, const QString &error) {
    auto *module = qobject_cast<IModule *>(sender());
    for (auto it = m_stops.begin(); it != m_stops.end(); ++it) {
        if ((it->phase != StopPhase::Stopping && it->phase != StopPhase::Failed) ||
            it->req_id != req_id || !it->pending.remove(module)) {
            continue;
        }
        if (!error.isEmpty()) {
            it->error =
                it->error.isEmpty()
                    ? QStringLiteral("Shutdown failed; plugin remains disabled and loaded: %1")
                          .arg(error)
                    : it->error + QLatin1Char('\n') + error;
            it->phase = StopPhase::Failed;
            emit pluginStopFinished(it.key(), false, it->error);
        } else if (it->phase == StopPhase::Failed) {
            emit enabledPluginsChanged();
        }
        return;
    }
}

void PluginRegistry::advancePluginStops() {
    auto ids = m_stops.keys();
    bool waiting = false;
    for (const auto &id : ids) {
        if (m_stops.value(id).phase == StopPhase::WaitingFrames) {
            waiting = true;
            auto *host = dynamic_cast<HostImpl *>(Host::get());
            auto modules = m_records.value(id).modules;
            // A module shuts down only once no account UI uses a service it supplied.
            if (host != nullptr && std::ranges::any_of(modules, [host](IModule *module) {
                    return !host->callerFramesIdle(module) || !host->servicesReleased(module);
                })) {
                continue;
            }
            requestModuleShutdown(id);
        }
        auto &stop = m_stops[id];
        if (stop.phase != StopPhase::Stopping) {
            continue;
        }
        waiting = true;
        if (!stop.pending.isEmpty()) {
            continue;
        }
        if (stop.checkpoint == StopCheckpoint::Pending) {
            continue;
        }
        // Failed checkpoints keep final-write handles and native mappings available for retry.
        if (stop.checkpoint != StopCheckpoint::Saved || !stop.error.isEmpty()) {
            stop.phase = StopPhase::Failed;
            emit enabledPluginsChanged();
            emit pluginStopFinished(id, false, stop.error);
            continue;
        }
        if (stop.error.isEmpty() && !drainPluginCallers(id)) {
            continue;
        }
        if (!stop.error.isEmpty()) {
            stop.phase = StopPhase::Failed;
            stop.error = QStringLiteral("Shutdown failed; plugin remains disabled and loaded: %1")
                             .arg(stop.error);
            emit pluginStopFinished(id, false, stop.error);
            continue;
        }
        finishPluginShutdown(id);
    }
    if (!waiting) {
        m_stop_timer.stop();
    }
}

auto PluginRegistry::drainPluginCallers(const QString &plugin_id) -> bool {
    auto modules = m_records.value(plugin_id).modules;
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    auto &stop = m_stops[plugin_id];
    if (!stop.callers_dropped) {
        for (auto *module : modules) {
            if (host != nullptr && !host->callerFramesIdle(module)) {
                return false;
            }
        }
        for (auto *module : modules) {
            if (host != nullptr) {
                host->dropCaller(module);
            }
        }
        stop.callers_dropped = true;
    }
    for (auto *module : modules) {
        if (host != nullptr && !host->callerDrained(module)) {
            return false;
        }
    }
    for (auto *module : modules) {
        if (host != nullptr) {
            stop.error += host->callerDrainError(module);
        }
    }
    return true;
}

void PluginRegistry::finishPluginShutdown(const QString &plugin_id) {
    auto modules = m_records.value(plugin_id).modules;
    for (auto *module : modules) {
        deactivateModule(module);
        if (auto *host = dynamic_cast<HostImpl *>(Host::get()); host != nullptr) {
            host->destroyClosedUi(module);
        }
    }
    syncPluginTranslations(plugin_id, false);
    auto &record = m_records[plugin_id];
    record.modules.clear();
    record.plugin = nullptr;
    record.plugin_object = nullptr;
    m_plugin_order.removeAll(plugin_id);
    rebuildPluginList();
    if (!unloadPlugin(plugin_id)) {
        m_stops[plugin_id].phase = StopPhase::Failed;
        m_stops[plugin_id].error =
            QStringLiteral("Unload failed; data and bundle retained. Retry removal.");
        rebuildPluginList();
        emit pluginStopFinished(plugin_id, false, m_stops.value(plugin_id).error);
        return;
    }
    if (!m_stops.value(plugin_id).remove) {
        m_stops.remove(plugin_id);
        emit enabledPluginsChanged();
        emit pluginStopFinished(plugin_id, false, QString());
        return;
    }
    m_stops[plugin_id].phase = StopPhase::Cleaning;
    ++m_state_revision;
    m_state_requests.append({.action = PluginStateAction::RemoveData,
                             .plugin_id = plugin_id,
                             .request = m_stops.value(plugin_id).req_id,
                             .revision = m_state_revision});
    startPluginStateJob();
}

void PluginRegistry::onPluginDataRemoved(const QString &plugin_id, const QString &error) {
    if (!m_stops.contains(plugin_id)) {
        return;
    }
    if (error.isEmpty()) {
        m_stops.remove(plugin_id);
        m_records.remove(plugin_id);
    } else {
        m_stops[plugin_id].phase = StopPhase::Failed;
        m_stops[plugin_id].error = error;
    }
    rebuildPluginList();
    emit enabledPluginsChanged();
    emit pluginStopFinished(plugin_id, error.isEmpty(), error);
}

bool PluginRegistry::unloadPlugin(const QString &plugin_id) {
    auto recordIt = m_records.find(plugin_id);
    if (recordIt == m_records.end()) {
        return false;
    }

    auto &record = *recordIt;
    if (record.library == nullptr) {
        return true;
    }

    bool ok = record.library->unload();
    if (!ok) {
        qWarning() << "Plugin unload error:" << plugin_id << record.library->errorString();
        return false;
    }

    delete record.library;
    record.library = nullptr;
    return true;
}

void PluginRegistry::activateModule(IModule *module, const QString &plugin_id) {
    QPointer<PluginRegistry> alive(this);
    if (module == nullptr || plugin_id.isEmpty() || m_stops.contains(plugin_id)) {
        return;
    }
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    if (host != nullptr) {
        host->enterFrame(module);
        host->setCallerClosing(module, false);
    }
    auto frame = qScopeGuard([host, module] {
        if (host != nullptr) {
            host->leaveFrame(module);
        }
    });

    auto record = m_records.constFind(plugin_id);
    if (record == m_records.constEnd()) {
        qWarning() << "Module activation has no plugin record:" << plugin_id;
        return;
    }
    m_module_owner.insert(
        module,
        {.plugin_id = plugin_id,
         .module_id = QString::fromUtf8(module->meta().value(QStringLiteral("id")).toUtf8()),
         .plugin_hash = record->external ? record->plugin_hash : QString(),
         .external = record->external});
    if (m_signer_polling) {
        module->setSignerPolling(true);
    }
    connect(module, &IModule::instances, this, &PluginRegistry::onModuleInstances, qontrol::UNIQUE);
    connect(module, QOverload<ReqId, QStringList>::of(&IModule::autoStart), this,
            &PluginRegistry::onModuleAutoStart, qontrol::UNIQUE);
    connect(module, &IModule::error, this, &PluginRegistry::onModuleError, qontrol::UNIQUE);
    connect(module, &IModule::accountDeleted, this, &PluginRegistry::onAccountDeleted,
            // Leave plugin code's stack before completion can permit library unload.
            static_cast<Qt::ConnectionType>(qontrol::UNIQUE | Qt::QueuedConnection));

    ReqId autoReqId = module->autoStart();
    if (alive == nullptr) {
        return;
    }
    if (autoReqId != 0) {
        m_pending_auto_start_requests[module].insert(autoReqId);
        if (m_sync_auto_start_responses.contains(module) &&
            m_sync_auto_start_responses.value(module).first == autoReqId) {
            auto response = m_sync_auto_start_responses.take(module);
            applyModuleAutoStart(module, response.first, response.second);
        }
    }

    if (!m_stops.contains(plugin_id)) {
        requestModuleInstances(module);
    }
}

void PluginRegistry::requestModuleInstances(IModule *module) {
    QPointer<PluginRegistry> alive(this);
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    if (host != nullptr) {
        host->enterFrame(module);
    }
    auto frame = qScopeGuard([host, module] {
        if (host != nullptr) {
            host->leaveFrame(module);
        }
    });
    ReqId listReqId = module->list();
    if (alive == nullptr) {
        return;
    }
    if (listReqId == 0) {
        return;
    }
    m_pending_list_requests[module].insert(listReqId);
    if (m_sync_list_responses.contains(module) &&
        m_sync_list_responses.value(module).first == listReqId) {
        auto response = m_sync_list_responses.take(module);
        applyModuleInstances(module, response.first, response.second);
    }
}

void PluginRegistry::deactivateModule(IModule *module) {
    if (module == nullptr) {
        return;
    }

    disconnect(module, &IModule::instances, this, &PluginRegistry::onModuleInstances);
    disconnect(module, QOverload<ReqId, QStringList>::of(&IModule::autoStart), this,
               &PluginRegistry::onModuleAutoStart);
    disconnect(module, &IModule::error, this, &PluginRegistry::onModuleError);
    disconnect(module, &IModule::accountDeleted, this, &PluginRegistry::onAccountDeleted);

    m_pending_list_requests.remove(module);
    m_pending_auto_start_requests.remove(module);
    m_sync_list_responses.remove(module);
    m_sync_auto_start_responses.remove(module);
    m_launcher_instances.remove(module);
    m_module_owner.remove(module);

    // A module that goes away must not leave a request pinning polling on forever.
    if (auto *host = Host::get(); host != nullptr) {
        host->requestSignerPolling(module, false);
    }
    if (auto *hostImpl = dynamic_cast<HostImpl *>(Host::get()); hostImpl != nullptr) {
        hostImpl->dropCaller(module);
    }
}

bool PluginRegistry::startOwnedInstance(IModule *module, const QString &instance_id) {
    if (module == nullptr) {
        return false;
    }

    QString trimmedInstanceId = instance_id.trimmed();
    if (!caller(module).has_value() || trimmedInstanceId.isEmpty()) {
        return false;
    }

    for (const auto &requests : m_pending_deletions) {
        if (requests.values().contains(trimmedInstanceId)) {
            return false;
        }
    }

    auto *host = dynamic_cast<HostImpl *>(Host::get());
    if (host != nullptr) {
        host->enterFrame(module);
    }
    auto frame = qScopeGuard([host, module] {
        if (host != nullptr) {
            host->leaveFrame(module);
        }
    });
    module->startInstance(trimmedInstanceId);
    return true;
}

void PluginRegistry::stopPluginInstances(const QString &plugin_id) {
    auto *host = Host::get();
    if (host == nullptr) {
        return;
    }

    auto *hostImpl = dynamic_cast<HostImpl *>(host);
    if (hostImpl == nullptr) {
        return;
    }

    QSet<QString> ownedIds;
    for (auto it = m_instance_owner_by_id.constBegin(); it != m_instance_owner_by_id.constEnd();
         ++it) {
        if (it.value() == plugin_id) {
            ownedIds.insert(it.key());
        }
    }

    if (ownedIds.isEmpty()) {
        return;
    }

    for (const auto &instanceId : ownedIds) {
        hostImpl->removeInstance(instanceId);

        bool stillLive = false;
        auto afterRemove = hostImpl->liveInstances();
        for (auto *live : afterRemove) {
            if (live != nullptr && live->id() == instanceId) {
                stillLive = true;
                break;
            }
        }
        if (stillLive) {
            qWarning() << "Owned plugin instance still live after stop/remove:" << plugin_id
                       << instanceId;
            continue;
        }
        m_instance_owner_by_id.remove(instanceId);
    }
}

void PluginRegistry::onHostInstanceRegistered(const QString &id) {
    QString instanceId = id.trimmed();
    if (instanceId.isEmpty()) {
        return;
    }

    // The host records the registering module, so an instance a module brings up on
    // its own (a connected device) is attributed like a host-started one.
    auto *hostImpl = dynamic_cast<HostImpl *>(Host::get());
    if (hostImpl == nullptr) {
        return;
    }
    auto ownerIt = m_module_owner.constFind(hostImpl->instanceOwner(instanceId));
    if (ownerIt == m_module_owner.constEnd()) {
        return;
    }
    m_instance_owner_by_id.insert(instanceId, ownerIt->plugin_id);
    hostImpl->registerPriceSource(instanceId, ownerIt->plugin_id);
}

void PluginRegistry::relistModules() {
    const auto modules = enabledModulesForLauncher();
    for (auto *module : modules) {
        if (module == nullptr || !m_module_owner.contains(module)) {
            continue;
        }
        requestModuleInstances(module);
    }
}

void PluginRegistry::onLauncherRefreshRequested() {
    relistModules();
}

void PluginRegistry::onSignerPollingChanged(bool enabled) {
    m_signer_polling = enabled;
    // Every enabled module is told, not just the ones holding instances: a module
    // that owns a device service must start it before it has any device to show.
    for (auto it = m_module_owner.constBegin(); it != m_module_owner.constEnd(); ++it) {
        if (it.key() != nullptr && caller(it.key()).has_value()) {
            it.key()->setSignerPolling(enabled);
        }
    }
}

void PluginRegistry::onHostInstanceRemoved(const QString &id) {
    m_instance_owner_by_id.remove(id.trimmed());
}

void PluginRegistry::rebuildPluginList() {
    qDeleteAll(m_metadata_plugins);
    m_metadata_plugins.clear();
    m_listed_plugins.clear();

    for (const auto &id : m_plugin_order) {
        auto recordIt = m_records.find(id);
        if (recordIt != m_records.end() && recordIt->plugin != nullptr) {
            m_listed_plugins.append(recordIt->plugin);
        }
    }

    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        if (it->builtin || !it->discovered || it->plugin != nullptr) {
            continue;
        }

        auto meta = it->meta;
        if (!meta.contains(QStringLiteral("id"))) {
            meta.insert(QStringLiteral("id"), it.key());
        }
        if (!meta.contains(QStringLiteral("name")) && !it->name.isEmpty()) {
            meta.insert(QStringLiteral("name"), it->name);
        }
        if (!meta.contains(QStringLiteral("version")) && !it->version.isEmpty()) {
            meta.insert(QStringLiteral("version"), it->version);
        }

        auto *placeholder = new MetadataOnlyPlugin(meta);
        m_metadata_plugins.append(placeholder);
        m_listed_plugins.append(placeholder);
    }
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
