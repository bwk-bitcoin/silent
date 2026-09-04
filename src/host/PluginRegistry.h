#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QPluginLoader>
#include <QSet>
#include <QString>
#include <optional>

#include "interfaces/module.h"

class PluginRegistry final : public QObject {
    Q_OBJECT

public:
    explicit PluginRegistry(QObject *parent = nullptr);
    ~PluginRegistry() override;

    struct CreatorLauncher {
        IModule *module = nullptr;
        QString id;
        QString name;
    };

    void registerBuiltin(IPlugin *plugin);
    void rescanDiscoveredPlugins();
    void reloadEnabledPlugins();
    void setEnabled(const QString &plugin_id, bool enabled);

    [[nodiscard]] static bool isCreatorInstanceId(const QString &id);
    [[nodiscard]] QList<IPlugin *> plugins() const;
    [[nodiscard]] QList<IModule *> enabledModulesForLauncher() const;
    [[nodiscard]] QList<QPair<QString, QString>> launcherInstancesForModule(const IModule *module) const;
    [[nodiscard]] QString instanceOwnerPluginId(const QString &id) const;
    [[nodiscard]] QList<CreatorLauncher> externalCreatorLaunchers() const;
    bool startLauncherInstance(IModule *module, const QString &instance_id);
    void deleteInstance(IModule *module, const QString &instance_id);
    [[nodiscard]] bool isPluginEnabled(const QString &plugin_id) const;
    [[nodiscard]] bool isPluginBuiltin(const QString &plugin_id) const;
    [[nodiscard]] bool isBuiltinModule(IModule *module) const;
    [[nodiscard]] bool pluginHasBuiltinModules(const QString &plugin_id) const;

signals:
    void enabledPluginsChanged();

private slots:
    void onModuleInstances(ReqId req_id, QList<QPair<QString, QString>> instances);
    void onModuleAutoStart(ReqId req_id, QStringList ids);
    void onModuleError(std::optional<ReqId> req_id, const QString &message);
    void onHostInstanceRegistered(const QString &id);
    void onHostInstanceRemoved(const QString &id);
    void onSignerPollingChanged(bool enabled);

private:
    struct PluginRecord {
        QString id;
        QString name;
        QString version;
        QString library_path;
        QString i18n_dir;
        QMap<QString, QString> meta;
        QPluginLoader *loader = nullptr;
        QObject *plugin_object = nullptr;
        IPlugin *plugin = nullptr;
        QList<IModule *> modules;
        bool enabled = false;
        bool builtin = false;
        bool external = false;
        bool discovered = false;
    };

    QString pluginId(IPlugin *plugin) const;
    QString pluginI18nDir(IPlugin *plugin) const;
    void syncPluginTranslations(const QString &plugin_id, bool enabled);
    [[nodiscard]] IPlugin *pluginById(const QString &plugin_id) const;
    bool enablePlugin(const QString &plugin_id);
    bool disablePlugin(const QString &plugin_id);
    bool unloadPlugin(const QString &plugin_id);
    void activateModule(IModule *module, IPlugin *plugin);
    void deactivateModule(IModule *module);
    bool startOwnedInstance(IModule *module, const QString &instance_id);
    void stopPluginInstances(const QString &plugin_id);
    [[nodiscard]] bool hasLiveOwnedInstances(const QString &plugin_id) const;
    void rebuildPluginList();
    [[nodiscard]] QString resolvePluginLibraryPath(const QString &plugin_dir, const QMap<QString, QString> &meta) const;

    QList<QString> m_plugin_order;
    QHash<QString, PluginRecord> m_records;
    QList<IPlugin *> m_listed_plugins;
    QList<IPlugin *> m_metadata_plugins;
    QSet<IModule *> m_builtin_modules;
    QHash<IModule *, QSet<ReqId>> m_pending_list_requests;
    QHash<IModule *, QSet<ReqId>> m_pending_auto_start_requests;
    QHash<IModule *, QList<QPair<QString, QString>>> m_launcher_instances;
    QHash<IModule *, IPlugin *> m_module_owner;
    QHash<QString, QString> m_instance_owner_by_id;
    QHash<IModule *, QPair<ReqId, QList<QPair<QString, QString>>>> m_sync_list_responses;
    QHash<IModule *, QPair<ReqId, QStringList>> m_sync_auto_start_responses;
    bool m_signer_polling = false;
};
