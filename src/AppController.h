#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <Qontrol>
#include <memory>
#include <optional>
#include <vector>
#include "interfaces/types.h"
#include "theme/Palette.h"
#include <silent.h>

class Host;
class IModule;
class IPlugin;
class PluginRegistry;
class QWidget;
class IThemeProvider;

struct RegtestDefaultsInfo {
    QString blindbit_url;
    QString electrum_url;
};

class AppController : public qontrol::Controller {
    Q_OBJECT

public:
    AppController();
    ~AppController() override;
    static auto init() -> void;
    static auto get() -> AppController *;
    auto accounts() -> int;
    [[nodiscard]] auto isAccountOpen(const QString &name) const -> bool;
    [[nodiscard]] auto regtestDefaults() const -> std::optional<RegtestDefaultsInfo>;
    [[nodiscard]] auto pluginRegistry() const -> PluginRegistry *;
    auto registerBuiltinPlugin(std::unique_ptr<IPlugin> plugin) -> void;
    [[nodiscard]] auto keysTab() const -> QWidget *;

signals:
    void accountList(QList<QString>);
    void accountCreated(const QString &name);
    void regtestDefaultsReady(const QString &blindbit, const QString &electrum);

public slots:
    auto initState() -> void;
    auto addAccount(const QString &name) -> void;
    auto removeAccount(const QString &account) -> void;
    auto listAccounts() -> void;
    auto createAccount(const QString &name, const QString &descriptor, Network network,
                       const QString &blindbit_url, const QString &electrum_url) -> void;
    auto onAccountCreated(const QString &name) -> void;
    auto openAccount(const QString &name) -> void;
    auto deleteAccount(const QString &name) -> void;
    auto openAppSettings() -> void;
    auto onDeleteConfirmed(const QString &account) -> void;
    auto onHostInstanceRemoved(const QString &id) -> void;
    auto onHostInstanceRegistered(const QString &id) -> void;
    auto onRegtestDefaultsReady(const QString &blindbit, const QString &electrum) -> void;
    auto onHostSettingsSectionsChanged() -> void;
    auto onPluginRegistryChanged() -> void;

private:
    auto finalizeListAccounts() -> void;
    auto pluginIdForModule(const IModule *module) const -> QString;
    [[nodiscard]] auto moduleForPluginId(const QString &plugin_id) const -> IModule *;
    [[nodiscard]] static auto pluginIdForAccount(const QString &account_id) -> QString;
    auto applyPersistedThemeAtStartup() -> void;
    auto requestPersistedThemePalette() -> void;
    auto onStartupThemeNames(ReqId req_id, const QStringList &themes) -> void;
    auto onStartupThemePalette(ReqId req_id, theme::Palette palette) -> void;

    QSet<QString> m_open_accounts;
    std::optional<RegtestDefaultsInfo> m_regtest_defaults;
    std::unique_ptr<PluginRegistry> m_plugin_registry;
    std::vector<std::unique_ptr<IPlugin>> m_builtin_plugins;
    Host *m_host = nullptr;

    QList<QString> m_config_order;
    QSet<QString> m_known_configs;
    QSet<QString> m_discovered_accounts;

    QString m_startup_active_theme;
    bool m_startup_custom_theme_applied = false;
    QList<IThemeProvider *> m_startup_theme_providers;
    QHash<ReqId, IThemeProvider *> m_startup_pending_theme_list;
    QHash<ReqId, IThemeProvider *> m_startup_pending_palette;
};
