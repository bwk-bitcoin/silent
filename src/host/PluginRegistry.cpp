#include "PluginRegistry.h"
#include "Host.h"
#include "i18n/Tr.h"
#include "interfaces/instance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <common.h>
#include <silent.h>

namespace {
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

QMap<QString, QString> readSidecarMeta(const QString &path) {
    QFile sidecar(path);
    if (!sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Plugin sidecar open failed:" << path << sidecar.errorString();
        return {};
    }

    QJsonParseError parseError;
    auto doc = QJsonDocument::fromJson(sidecar.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Plugin sidecar parse failed:" << path << parseError.errorString();
        return {};
    }

    QMap<QString, QString> meta;
    auto object = doc.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isString()) {
            meta.insert(it.key(), it.value().toString());
            continue;
        }
        if (it.value().isDouble()) {
            meta.insert(it.key(), QString::number(it.value().toDouble()));
            continue;
        }
        if (it.value().isBool()) {
            meta.insert(it.key(),
                        it.value().toBool() ? QStringLiteral("true") : QStringLiteral("false"));
            continue;
        }
        if (it.value().isArray()) {
            QStringList values;
            auto array = it.value().toArray();
            for (const auto &entry : array) {
                if (entry.isString()) {
                    values.append(entry.toString());
                }
            }
            meta.insert(it.key(), values.join(','));
        }
    }

    return meta;
}

} // namespace

PluginRegistry::PluginRegistry(QObject *parent) : QObject(parent) {
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
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if (!it->external) {
            continue;
        }
        disablePlugin(it.key());
        if (it->loader != nullptr) {
            delete it->loader;
            it->loader = nullptr;
        }
    }
    qDeleteAll(m_metadata_plugins);
    m_metadata_plugins.clear();
}

auto PluginRegistry::rescanDiscoveredPlugins() -> int {
    QSet<QString> known;
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if (!it->external) {
            continue;
        }
        if (it->discovered) {
            known.insert(it.key());
        }
        it->discovered = false;
        // A scan gives a plugin that failed to load another chance.
        if (it->plugin == nullptr) {
            it->load_error.clear();
        }
    }

    QString pluginsRoot = QDir::homePath() + QStringLiteral("/.silent/plugins");
    QDir rootDir(pluginsRoot);
    // A missing folder only means no plugin is installed yet.
    m_available = !rootDir.exists() || QFileInfo(pluginsRoot).isReadable();
    if (!rootDir.exists()) {
        rebuildPluginList();
        emit enabledPluginsChanged();
        return 0;
    }

    int found = 0;
    auto pluginDirs = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &pluginDirInfo : pluginDirs) {
        QString pluginDir = pluginDirInfo.absoluteFilePath();
        QString pluginId = pluginDirInfo.fileName().trimmed();
        if (pluginId.isEmpty()) {
            continue;
        }

        QString sidecarPath = pluginDir + QStringLiteral("/%1.json").arg(pluginId);
        auto meta = readSidecarMeta(sidecarPath);
        if (meta.isEmpty()) {
            continue;
        }

        QString metaId = meta.value(QStringLiteral("id")).trimmed();
        if (metaId.isEmpty() || metaId != pluginId) {
            qWarning() << "Plugin sidecar id mismatch:" << sidecarPath;
            continue;
        }

        auto &record = m_records[pluginId];
        record.id = pluginId;
        record.external = true;
        record.name = meta.value(QStringLiteral("name")).trimmed();
        record.version = meta.value(QStringLiteral("version")).trimmed();
        record.meta = meta;
        record.i18n_dir = pluginDir + QStringLiteral("/i18n");
        record.library_path = resolvePluginLibraryPath(pluginDir, meta);
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

    rebuildPluginList();
    emit enabledPluginsChanged();
    return found;
}

void PluginRegistry::reloadEnabledPlugins() {
    QHash<QString, bool> desiredEnabled;
    auto enabled = app_enabled_plugins();
    for (auto pluginId : enabled) {
        desiredEnabled.insert(QString::fromStdString(std::string(pluginId.c_str())), true);
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
        if (m_records.value(id).enabled) {
            if (!enablePlugin(id)) {
                // Keep runtime enabled state truthful when load fails.
                if (m_records.value(id).enabled) {
                    changed = true;
                }
                m_records[id].enabled = false;
                ::app_set_plugin_enabled(rust::String(id.toStdString()), false);
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
    record.plugin = plugin;
    record.enabled = true;
    if (!m_plugin_order.contains(id)) {
        m_plugin_order.append(id);
    }
    syncPluginTranslations(id, true);
    if (hadEntry && !wasEnabled) {
        ::app_set_plugin_enabled(rust::String(id.toStdString()), true);
    }

    QList<IModule *> modules = plugin->modules();
    std::ranges::remove(modules, nullptr);
    record.modules = modules;
    for (auto *module : modules) {
        m_builtin_modules.insert(module);
        activateModule(module, plugin);
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
    auto *plugin = m_module_owner.value(module, nullptr);
    if (plugin == nullptr) {
        return std::nullopt;
    }
    QString id = pluginId(plugin);
    bool external = !isBuiltinModule(module);
    return plugin::Caller{
        .moduleId = module->meta().value(QStringLiteral("id")),
        .pluginId = id,
        .external = external,
        .pluginHash = external ? pluginHash(id) : QString(),
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
    QString trimmedId = instance_id.trimmed();
    if (trimmedId.isEmpty()) {
        return;
    }

    // removeInstance stops the instance and ignores an id the host does not hold.
    if (auto *hostImpl = dynamic_cast<HostImpl *>(Host::get()); hostImpl != nullptr) {
        hostImpl->removeInstance(trimmedId);
    }
    m_instance_owner_by_id.remove(trimmedId);

    // The module owns its persistence: ask it to drop the stored state.
    if (module != nullptr) {
        module->deleteInstance(trimmedId);
    }

    emit enabledPluginsChanged();
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
    if (plugin_id.trimmed().isEmpty()) {
        return false;
    }

    if (pluginHasBuiltinModules(plugin_id) && !enabled) {
        return false;
    }

    bool previous = isPluginEnabled(plugin_id);
    if (previous == enabled) {
        return true;
    }

    if (enabled) {
        m_records[plugin_id].enabled = true;
        if (!enablePlugin(plugin_id)) {
            m_records[plugin_id].enabled = false;
            return false;
        }
    } else {
        if (!disablePlugin(plugin_id)) {
            m_records[plugin_id].enabled = true;
            return false;
        }
        m_records[plugin_id].enabled = false;
    }

    syncPluginTranslations(plugin_id, enabled);
    ::app_set_plugin_enabled(rust::String(plugin_id.toStdString()), enabled);
    emit enabledPluginsChanged();
    return true;
}

auto PluginRegistry::isAvailable() const -> bool {
    return m_available;
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
    if (record.loader != nullptr) {
        qWarning()
            << "Enable blocked: plugin still has active loader; disable/unload cleanup required"
            << plugin_id;
        return false;
    }

    if (record.library_path.isEmpty()) {
        qWarning() << "Enable failed: plugin library missing" << plugin_id;
        record.load_error = TR("settings-plugin-no-library");
        return false;
    }

    // Hashed before the load, so the hash names the code that runs.
    QFile library(record.library_path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!library.open(QIODevice::ReadOnly) || !hash.addData(&library)) {
        qWarning() << "Enable failed: plugin library unreadable" << plugin_id;
        record.load_error = library.errorString();
        return false;
    }
    library.close();
    record.plugin_hash = QString::fromLatin1(hash.result().toHex());

    record.loader = new QPluginLoader(record.library_path);
    if (!record.loader->load()) {
        qWarning() << "Plugin load failed:" << plugin_id << record.loader->errorString();
        record.load_error = record.loader->errorString();
        delete record.loader;
        record.loader = nullptr;
        return false;
    }

    QObject *root = record.loader->instance();
    if (root == nullptr) {
        qWarning() << "Plugin instance failed:" << plugin_id << record.loader->errorString();
        record.load_error = record.loader->errorString();
        record.loader->unload();
        delete record.loader;
        record.loader = nullptr;
        return false;
    }

    auto *plugin = qobject_cast<IPlugin *>(root);
    if (plugin == nullptr) {
        qWarning() << "Plugin interface cast failed:" << plugin_id << record.loader->errorString();
        record.load_error = TR("settings-plugin-not-silent");
        record.loader->unload();
        delete record.loader;
        record.loader = nullptr;
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
        activateModule(module, plugin);
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

    auto &record = *recordIt;
    if (record.plugin == nullptr) {
        bool unloaded = unloadPlugin(plugin_id);
        if (!unloaded) {
            qWarning() << "Plugin unload failed:" << plugin_id;
            return false;
        }
        // Disabling tears down/unloads only; discovery state is owned by
        // rescanDiscoveredPlugins(), so the plugin stays listed as discovered+disabled.
        syncPluginTranslations(plugin_id, false);
        rebuildPluginList();
        return true;
    }

    stopPluginInstances(plugin_id);
    if (hasLiveOwnedInstances(plugin_id)) {
        qWarning() << "Plugin disable aborted: live instances still owned by plugin:" << plugin_id;
        return false;
    }

    for (auto *module : record.modules) {
        deactivateModule(module);
    }

    bool unloaded = unloadPlugin(plugin_id);
    if (!unloaded) {
        qWarning() << "Plugin unload failed:" << plugin_id;
        // Roll back module wiring best-effort so runtime state stays internally consistent.
        for (auto *module : record.modules) {
            activateModule(module, record.plugin);
        }
        return false;
    }

    m_plugin_order.removeAll(plugin_id);
    record.modules.clear();
    record.plugin = nullptr;
    record.plugin_object = nullptr;
    // Disabling tears down/unloads only; discovery state is owned by
    // rescanDiscoveredPlugins(), so the plugin stays listed as discovered+disabled.

    syncPluginTranslations(plugin_id, false);
    rebuildPluginList();
    return true;
}

bool PluginRegistry::unloadPlugin(const QString &plugin_id) {
    auto recordIt = m_records.find(plugin_id);
    if (recordIt == m_records.end()) {
        return false;
    }

    auto &record = *recordIt;
    if (record.loader == nullptr) {
        return true;
    }

    bool ok = record.loader->unload();
    if (!ok) {
        qWarning() << "Plugin unload error:" << plugin_id << record.loader->errorString();
        return false;
    }

    delete record.loader;
    record.loader = nullptr;
    return true;
}

void PluginRegistry::activateModule(IModule *module, IPlugin *plugin) {
    if (module == nullptr) {
        return;
    }

    m_module_owner.insert(module, plugin);
    if (m_signer_polling) {
        module->setSignerPolling(true);
    }
    connect(module, &IModule::instances, this, &PluginRegistry::onModuleInstances, qontrol::UNIQUE);
    connect(module, QOverload<ReqId, QStringList>::of(&IModule::autoStart), this,
            &PluginRegistry::onModuleAutoStart, qontrol::UNIQUE);
    connect(module, &IModule::error, this, &PluginRegistry::onModuleError, qontrol::UNIQUE);

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
    module->setSignerPolling(false);
}

bool PluginRegistry::startOwnedInstance(IModule *module, const QString &instance_id) {
    if (module == nullptr) {
        return false;
    }

    auto ownerIt = m_module_owner.find(module);
    if (ownerIt == m_module_owner.end() || ownerIt.value() == nullptr) {
        return false;
    }

    QString ownerPluginId = pluginId(ownerIt.value());
    QString trimmedInstanceId = instance_id.trimmed();
    if (ownerPluginId.isEmpty() || trimmedInstanceId.isEmpty()) {
        return false;
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

bool PluginRegistry::hasLiveOwnedInstances(const QString &plugin_id) const {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    if (host == nullptr) {
        return false;
    }

    auto liveInstances = host->liveInstances();
    return std::ranges::any_of(liveInstances, [this, &plugin_id](const auto *instance) {
        return instance != nullptr && m_instance_owner_by_id.value(instance->id()) == plugin_id;
    });
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
    auto *ownerPlugin = m_module_owner.value(hostImpl->instanceOwner(instanceId), nullptr);
    if (ownerPlugin == nullptr) {
        return;
    }
    QString ownerPluginId = pluginId(ownerPlugin);
    if (ownerPluginId.isEmpty()) {
        return;
    }
    m_instance_owner_by_id.insert(instanceId, ownerPluginId);
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
        if (it.key() != nullptr) {
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
QString PluginRegistry::resolvePluginLibraryPath(const QString &plugin_dir,
                                                 const QMap<QString, QString> &meta) const {
    QString explicitLibrary = meta.value(QStringLiteral("library")).trimmed();
    if (!explicitLibrary.isEmpty()) {
        QFileInfo explicitInfo(QDir(plugin_dir).absoluteFilePath(explicitLibrary));
        if (explicitInfo.exists() && explicitInfo.isFile()) {
            return explicitInfo.absoluteFilePath();
        }
    }

    QString pluginId = meta.value(QStringLiteral("id")).trimmed();
    if (!pluginId.isEmpty()) {
#ifdef Q_OS_MAC
        QString candidate =
            QDir(plugin_dir).absoluteFilePath(QStringLiteral("lib%1.dylib").arg(pluginId));
#elif defined(Q_OS_WIN)
        QString candidate =
            QDir(plugin_dir).absoluteFilePath(QStringLiteral("%1.dll").arg(pluginId));
#else
        QString candidate =
            QDir(plugin_dir).absoluteFilePath(QStringLiteral("lib%1.so").arg(pluginId));
#endif
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
            return candidate;
        }
    }

#ifdef Q_OS_MAC
    QStringList filters{QStringLiteral("*.dylib")};
#elif defined(Q_OS_WIN)
    QStringList filters{QStringLiteral("*.dll")};
#else
    QStringList filters{QStringLiteral("*.so")};
#endif

    QDir dir(plugin_dir);
    auto candidates = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    if (!candidates.isEmpty()) {
        return candidates.first().absoluteFilePath();
    }

    return {};
}
