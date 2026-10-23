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
#include <QSet>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <common.h>
#include <silent.h>

namespace {
constexpr int PLUGIN_STOP_POLL_INTERVAL_MS = 20;

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
    connect(this, &PluginRegistry::pluginStopPrepared, this, &PluginRegistry::onPluginStopPrepared,
            qontrol::UNIQUE);
    connect(this, &PluginRegistry::pluginDataRemoved, this, &PluginRegistry::onPluginDataRemoved,
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
    rescanDiscoveredPlugins();
    reloadEnabledPlugins();
}

PluginRegistry::~PluginRegistry() {
    for (auto *worker : findChildren<QThread *>(QString(), Qt::FindDirectChildrenOnly)) {
        worker->wait();
    }
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

auto PluginRegistry::rescanDiscoveredPlugins() -> int {
    auto removals = ::app_removing_plugins();
    if (!removals.is_ok) {
        m_removal_read_error = QString::fromStdString(std::string(removals.error));
        emit enabledPluginsChanged();
        return 0;
    }
    m_removal_read_error.clear();
    m_enabled_snapshot.clear();
    for (const auto &id : removals.enabled_plugins) {
        m_enabled_snapshot.insert(QString::fromStdString(std::string(id)));
    }
    QSet<QString> known;
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if (!it->external) {
            continue;
        }
        if (it->discovered) {
            known.insert(it.key());
        }
        it->discovered = false;
        it->removal_phase.reset();
        // A scan gives a plugin that failed to load another chance.
        if (it->plugin == nullptr) {
            it->load_error.clear();
        }
    }

    // Only the plugins sealed in the app state: nothing found loose in
    // ~/.silent/plugins/ is ever read (ws-plaude/silent#41).
    int found = 0;
    for (const auto &bundle : ::plugin_metadata()) {
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
        if (!known.contains(pluginId)) {
            ++found;
        }

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
    emit enabledPluginsChanged();
    return found;
}

void PluginRegistry::reloadEnabledPlugins() {
    rescanDiscoveredPlugins();
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

    bool changed = false;
    for (auto it = desiredEnabled.constBegin(); it != desiredEnabled.constEnd(); ++it) {
        if (m_stops.contains(it.key())) {
            continue;
        }
        auto &record = m_records[it.key()];
        if (record.enabled != it.value()) {
            changed = true;
        }
        record.enabled = it.value();
    }

    for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
        if (!it->external) {
            continue;
        }
        QString id = it.key();
        if (pluginStopping(id)) {
            continue;
        }
        if (m_records.value(id).enabled) {
            if (!enablePlugin(id)) {
                // Keep runtime enabled state truthful when load fails.
                if (m_records.value(id).enabled) {
                    changed = true;
                }
                m_records[id].enabled = false;
                auto error = ::app_set_plugin_enabled(rust::String(id.toStdString()), false);
                if (!error.empty()) {
                    qWarning() << "Could not persist failed-plugin disable:" << error.c_str();
                }
            }
            continue;
        }
        disablePlugin(id);
    }

    if (changed) {
        emit enabledPluginsChanged();
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

    bool hadEntry = existingIt != m_records.end();
    bool wasEnabled = hadEntry && existingIt->enabled;
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
    if (hadEntry && !wasEnabled) {
        auto error = ::app_set_plugin_enabled(rust::String(id.toStdString()), true);
        if (!error.empty()) {
            qWarning() << "Could not persist built-in enable state:" << error.c_str();
        }
    }

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
    return trimmed == QStringLiteral("create") || trimmed.endsWith(QStringLiteral(".create"));
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

    bool previous = isPluginEnabled(plugin_id);
    if (previous == enabled) {
        return true;
    }

    auto record = m_records.find(plugin_id);
    if (record == m_records.end() || (!record->discovered && !record->builtin)) {
        return false;
    }
    auto saved = ::app_set_plugin_enabled(rust::String(plugin_id.toStdString()), true);
    if (!saved.empty()) {
        record->load_error = QStringLiteral("Could not persist enabled state: %1")
                                 .arg(QString::fromUtf8(saved.data(), saved.size()));
        return false;
    }
    m_records[plugin_id].enabled = true;
    if (!enablePlugin(plugin_id)) {
        m_records[plugin_id].enabled = false;
        auto rollback = ::app_disable_plugin(rust::String(plugin_id.toStdString()));
        if (!rollback.empty()) {
            m_records[plugin_id].load_error +=
                QStringLiteral("\nCould not persist disabled state: %1")
                    .arg(QString::fromUtf8(rollback.data(), rollback.size()));
        }
        return false;
    }

    syncPluginTranslations(plugin_id, enabled);
    emit enabledPluginsChanged();
    return true;
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

bool PluginRegistry::enablePlugin(const QString &plugin_id) {
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
    auto opened = ::plugin_library(rust::String(plugin_id.toStdString()));
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
    for (auto *module : record.modules) {
        activateModule(module, plugin_id);
    }
    rebuildPluginList();
    syncPluginTranslations(plugin_id, true);
    return true;
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
    return m_stops.contains(plugin_id) && (m_stops.value(plugin_id).phase != StopPhase::Failed ||
                                           !m_stops.value(plugin_id).pending.isEmpty());
}

auto PluginRegistry::pluginDisableRetryable(const QString &plugin_id) const -> bool {
    auto stop = m_stops.constFind(plugin_id);
    return stop != m_stops.constEnd() && stop->phase == StopPhase::Failed && !stop->remove &&
           stop->pending.isEmpty();
}

auto PluginRegistry::pluginStopError(const QString &plugin_id) const -> QString {
    auto stop = m_stops.constFind(plugin_id);
    if (stop != m_stops.constEnd()) {
        if (stop->phase != StopPhase::Failed) {
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
    m_records[plugin_id].enabled = false;
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    for (auto *module : modules) {
        if (host != nullptr) {
            host->setCallerClosing(module, true);
        }
    }
    stopPluginInstances(plugin_id);
    emit enabledPluginsChanged();
    auto *worker = QThread::create([this, plugin_id, remove] {
        auto id = rust::String(plugin_id.toStdString());
        auto error = remove ? ::app_begin_plugin_removal(std::move(id))
                            : ::app_disable_plugin(std::move(id));
        emit pluginStopPrepared(plugin_id, QString::fromUtf8(error.data(), error.size()));
    });
    worker->setParent(this);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater, qontrol::UNIQUE);
    worker->start();
}

void PluginRegistry::onPluginStopPrepared(const QString &plugin_id, const QString &error) {
    if (!m_stops.contains(plugin_id)) {
        return;
    }
    auto &stop = m_stops[plugin_id];
    if (!error.isEmpty()) {
        stop.phase = StopPhase::Failed;
        m_records[plugin_id].enabled = false;
        stop.error =
            QStringLiteral(
                "Could not persist disabled/removal state; library and data retained. Plugin is "
                "unavailable in this session; retry. Restart state is not guaranteed: %1")
                .arg(error);
        requestModuleShutdown(plugin_id);
        auto failure = stop.error;
        emit enabledPluginsChanged();
        emit pluginStopFinished(plugin_id, false, failure);
        return;
    }
    stop.phase = StopPhase::Stopping;
    auto &record = m_records[plugin_id];
    record.enabled = false;
    if (stop.remove && !record.removal_phase.has_value()) {
        record.removal_phase = PluginRemovalPhase::Stopping;
    }
    requestModuleShutdown(plugin_id);
    emit enabledPluginsChanged();
    m_stop_timer.start();
}

void PluginRegistry::requestModuleShutdown(const QString &plugin_id) {
    auto modules = m_records.value(plugin_id).modules;
    auto request = m_stops.value(plugin_id).req_id;
    for (auto *module : modules) {
        m_stops[plugin_id].pending.insert(module);
        connect(module, &IModule::shutdownFinished, this, &PluginRegistry::onModuleShutdownFinished,
                static_cast<Qt::ConnectionType>(qontrol::UNIQUE | Qt::QueuedConnection));
    }
    for (auto *module : modules) {
        module->setSignerPolling(false);
        module->shutdown(request);
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
        auto &stop = m_stops[id];
        if (stop.phase != StopPhase::Stopping) {
            continue;
        }
        waiting = true;
        if (!stop.pending.isEmpty()) {
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
    auto *worker = QThread::create([this, plugin_id] {
        auto error = ::plugin_remove(rust::String(plugin_id.toStdString()));
        emit pluginDataRemoved(plugin_id, QString::fromUtf8(error.data(), error.size()));
    });
    worker->setParent(this);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater, qontrol::UNIQUE);
    worker->start();
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
    if (module == nullptr || plugin_id.isEmpty()) {
        return;
    }
    if (auto *host = dynamic_cast<HostImpl *>(Host::get()); host != nullptr) {
        host->setCallerClosing(module, false);
    }

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
    if (autoReqId != 0) {
        m_pending_auto_start_requests[module].insert(autoReqId);
        if (m_sync_auto_start_responses.contains(module) &&
            m_sync_auto_start_responses.value(module).first == autoReqId) {
            auto response = m_sync_auto_start_responses.take(module);
            applyModuleAutoStart(module, response.first, response.second);
        }
    }

    requestModuleInstances(module);
}

void PluginRegistry::requestModuleInstances(IModule *module) {
    ReqId listReqId = module->list();
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
