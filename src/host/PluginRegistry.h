#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QPluginLoader>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstdint>
#include <optional>

#include "host/PluginStateJob.h"
#include "interfaces/module.h"

class PluginLibrary;
enum class PluginRemovalPhase : std::uint8_t;

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
    // Requests a checked background snapshot; pluginStateChanged reports completion/loading.
    void rescanDiscoveredPlugins();
    void reloadEnabledPlugins();
    [[nodiscard]] auto pluginStateLoading() const -> bool;
    [[nodiscard]] auto pluginEnablePending(const QString &plugin_id) const -> bool;
    [[nodiscard]] auto pluginHasAccessHistory(const QString &plugin_id) const -> bool;
    // Both actions answer admission only. Runtime results arrive through the signals.
    auto setEnabled(const QString &plugin_id, bool enabled) -> bool;
    void removePlugin(const QString &plugin_id);
    [[nodiscard]] auto pluginStopping(const QString &plugin_id) const -> bool;
    [[nodiscard]] auto pluginStopPending(const QString &plugin_id) const -> bool;
    [[nodiscard]] auto pluginStopError(const QString &plugin_id) const -> QString;
    [[nodiscard]] auto pluginRemovalReadError() const -> QString;
    [[nodiscard]] auto pluginDisableRetryable(const QString &plugin_id) const -> bool;
    [[nodiscard]] auto pluginName(const QString &plugin_id) const -> QString;
    [[nodiscard]] auto pluginDescription(const QString &plugin_id) const -> QString;
    [[nodiscard]] auto pluginMetadata(const QString &plugin_id) const -> QMap<QString, QString>;
    // Why the plugin failed to load, empty when it did not fail.
    [[nodiscard]] auto pluginLoadError(const QString &plugin_id) const -> QString;
    // The open accounts that close with the plugin.
    [[nodiscard]] auto pluginAccountNames(const QString &plugin_id) const -> QStringList;

    [[nodiscard]] static bool isCreatorInstanceId(const QString &id);
    [[nodiscard]] QList<IPlugin *> plugins() const;
    [[nodiscard]] QList<IModule *> enabledModulesForLauncher() const;
    [[nodiscard]] QList<QPair<QString, QString>>
    launcherInstancesForModule(const IModule *module) const;
    [[nodiscard]] QString instanceOwnerPluginId(const QString &id) const;
    [[nodiscard]] QList<CreatorLauncher> externalCreatorLaunchers() const;
    bool startLauncherInstance(IModule *module, const QString &instance_id);
    void deleteInstance(IModule *module, const QString &instance_id);
    [[nodiscard]] auto accountDeletionPending(const QString &id) const -> bool;
    // Calls list() on every active module and applies each answer through
    // applyModuleInstances, the path activateModule uses.
    void relistModules();
    [[nodiscard]] bool isPluginEnabled(const QString &plugin_id) const;
    [[nodiscard]] bool isPluginBuiltin(const QString &plugin_id) const;
    [[nodiscard]] bool isBuiltinModule(IModule *module) const;
    [[nodiscard]] bool pluginHasBuiltinModules(const QString &plugin_id) const;
    // The sha256 (hex) of the external plugin's library as it was last loaded, empty
    // for a built-in plugin or one never loaded.
    [[nodiscard]] auto pluginHash(const QString &plugin_id) const -> QString;
    [[nodiscard]] auto pluginVersion(const QString &plugin_id) const -> QString;
    // Who `module` is, as the host stamps it on its IAccount and IKeys calls. Empty for
    // a module no active plugin owns.
    [[nodiscard]] auto caller(IModule *module) const -> std::optional<plugin::Caller>;

signals:
    void enabledPluginsChanged();
    void accountDeletionFinished(IModule *module, QString id, QString error);
    void pluginStopFinished(QString plugin_id, bool removed, QString error);
    void pluginStopPrepared(QString plugin_id, QString error);
    void pluginDataRemoved(QString plugin_id, QString error);
    void pluginStateChanged();
    void pluginEnableFinished(QString plugin_id, QString error);

public slots:
    void onPluginAccessSaved(uint64_t revision, const QString &error);

private slots:
    void onPluginStopPrepared(const QString &plugin_id, const QString &error);
    void onPluginDataRemoved(const QString &plugin_id, const QString &error);
    void onPluginStateJobFinished();
    void onPluginStateReadDeadline();
    void onModuleShutdownFinished(ReqId req_id, const QString &error);
    void advancePluginStops();
    void onAccountDeleted(ReqId req_id, const QString &id, const QString &error);
    void onModuleInstances(ReqId req_id, QList<QPair<QString, QString>> instances);
    void onModuleAutoStart(ReqId req_id, QStringList ids);
    void onModuleError(std::optional<ReqId> req_id, const QString &message);
    void onHostInstanceRegistered(const QString &id);
    void onHostInstanceRemoved(const QString &id);
    void onSignerPollingChanged(bool enabled);
    void onLauncherRefreshRequested();

protected:
    void beginPluginStop(const QString &plugin_id, bool remove);
    void startPluginStop(const QString &plugin_id, bool remove);
    void requestModuleShutdown(const QString &plugin_id);
    [[nodiscard]] auto drainPluginCallers(const QString &plugin_id) -> bool;
    void finishPluginShutdown(const QString &plugin_id);
    void requestPluginState(bool clear_errors);
    void startPluginStateJob();
    void finishPluginScan(const PluginStateJob &job);
    void applyPluginState(const PluginStateJob &job);
    void applyEnabledPlugins();
    void finishPluginEnable(const PluginStateJob &job);
    // A module that answers from inside list()/autoStart() replies before the pending
    // request is recorded, so the answer is parked and replayed by a direct call. The
    // slots resolve sender() and forward here; the replay passes the module it holds,
    // because sender() means nothing outside a signal delivery.
    void applyModuleInstances(IModule *module, ReqId req_id,
                              QList<QPair<QString, QString>> instances);
    void applyModuleAutoStart(IModule *module, ReqId req_id, QStringList ids);
    // Sends list() to one active module, replaying an answer it gave from inside list().
    void requestModuleInstances(IModule *module);

    [[nodiscard]] auto instanceName(const QList<IModule *> &modules,
                                    const QString &instance_id) const -> QString;

private:
    struct PluginRecord {
        QString id;
        QString name;
        QString version;
        QString i18n_dir;
        // An external plugin's catalogs, opened from its bundle, by file name.
        QHash<QString, QString> catalogs;
        QString load_error;
        // The library's sha256, taken before it was loaded.
        QString plugin_hash;
        QMap<QString, QString> meta;
        // The loaded library of an external plugin, null until loaded.
        PluginLibrary *library = nullptr;
        QObject *plugin_object = nullptr;
        IPlugin *plugin = nullptr;
        QList<IModule *> modules;
        bool enabled = false;
        bool builtin = false;
        bool external = false;
        bool discovered = false;
        std::optional<PluginRemovalPhase> removal_phase;
    };

    // Who an active module is, taken from its plugin record when it was activated. The
    // plugin's and the module's meta() are never read again for it (ws-plaude/silent#60).
    struct ModuleOwner {
        QString plugin_id;
        QString module_id;
        // The library's sha256, empty for a built-in module.
        QString plugin_hash;
        bool external = false;
    };

    QString pluginId(IPlugin *plugin) const;
    QString pluginI18nDir(IPlugin *plugin) const;
    void syncPluginTranslations(const QString &plugin_id, bool enabled);
    [[nodiscard]] IPlugin *pluginById(const QString &plugin_id) const;
    bool enablePlugin(const QString &plugin_id, const PluginLibraryResult &opened);
    bool disablePlugin(const QString &plugin_id);
    bool unloadPlugin(const QString &plugin_id);
    void activateModule(IModule *module, const QString &plugin_id);
    void deactivateModule(IModule *module);
    bool startOwnedInstance(IModule *module, const QString &instance_id);
    void stopPluginInstances(const QString &plugin_id);
    void rebuildPluginList();

    QList<QString> m_plugin_order;
    QHash<QString, PluginRecord> m_records;
    QList<IPlugin *> m_listed_plugins;
    QList<IPlugin *> m_metadata_plugins;
    QSet<IModule *> m_builtin_modules;
    QHash<IModule *, QSet<ReqId>> m_pending_list_requests;
    QHash<IModule *, QSet<ReqId>> m_pending_auto_start_requests;
    QHash<IModule *, QList<QPair<QString, QString>>> m_launcher_instances;
    QHash<IModule *, ModuleOwner> m_module_owner;
    QHash<QString, QString> m_instance_owner_by_id;
    QHash<IModule *, QPair<ReqId, QList<QPair<QString, QString>>>> m_sync_list_responses;
    QHash<IModule *, QPair<ReqId, QStringList>> m_sync_auto_start_responses;
    bool m_signer_polling = false;
    QHash<IModule *, QHash<ReqId, QString>> m_pending_deletions;
    ReqId m_delete_req_id = 0;
    enum class StopPhase { WaitingFrames, Stopping, Failed, Cleaning };
    enum class StopCheckpoint { Pending, Saved, Failed };

    struct PluginStop {
        StopPhase phase = StopPhase::WaitingFrames;
        bool remove = false;
        ReqId req_id = 0;
        QSet<IModule *> pending;
        QString error;
        StopCheckpoint checkpoint = StopCheckpoint::Pending;
        bool callers_dropped = false;
    };

    QHash<QString, PluginStop> m_stops;
    QString m_removal_read_error;
    // Desired enable state from the same checked snapshot as the removal phases.
    QSet<QString> m_enabled_snapshot;
    QSet<QString> m_access_snapshot;

    struct StateRequest {
        PluginStateAction action = PluginStateAction::Scan;
        QString plugin_id;
        quint64 request = 0;
        quint64 revision = 0;
        bool clear_errors = false;
    };

    QList<StateRequest> m_state_requests;
    QPointer<PluginStateJob> m_state_job;
    std::optional<PluginStateAction> m_state_consuming;
    QHash<QString, quint64> m_enabling;
    quint64 m_state_revision = 0;
    bool m_state_loaded = false;
    bool m_scan_requested = false;
    bool m_clear_load_errors = false;
    bool m_reload_requested = false;
    QTimer m_state_deadline;
    QTimer m_stop_timer;
};
