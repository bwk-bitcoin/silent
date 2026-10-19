#include "AppController.h"
#include "MainWindow.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"
#include "interfaces/theme.h"
#include "modules/keys/KeysModule.h"
#include "theme/Theme.h"
#include "views/modals/AppSettings.h"
#include "views/modals/ConfirmDelete.h"
#include <algorithm>
#include <common.h>
#include <qlogging.h>
#include <qthread.h>

AppController::AppController() = default;
AppController::~AppController() = default;

auto AppController::init() -> void {
    if (Controller::isInit()) {
        qFatal("Controller has already been initialized!");
    }
    Controller::init(new AppController);
}

auto AppController::get() -> AppController * {
    auto *ctrl = Controller::get();
    auto *controller = dynamic_cast<AppController *>(ctrl);
    return controller;
}

auto AppController::pluginRegistry() const -> PluginRegistry * {
    return m_plugin_registry.get();
}

auto AppController::keysTab() const -> QWidget * {
    auto *module = moduleForPluginId(QStringLiteral("keys"));
    auto *keysModule = dynamic_cast<keys::KeysModule *>(module);
    if (keysModule == nullptr) {
        return nullptr;
    }
    return keysModule->tab();
}

auto AppController::registerBuiltinPlugin(std::unique_ptr<IPlugin> plugin) -> void {
    if (!plugin) {
        return;
    }

    if (!m_plugin_registry) {
        m_plugin_registry = std::make_unique<PluginRegistry>();
    }

    auto *raw = plugin.get();
    m_builtin_plugins.push_back(std::move(plugin));
    m_plugin_registry->registerBuiltin(raw);
}

auto AppController::initState() -> void {
    if (!m_plugin_registry) {
        m_plugin_registry = std::make_unique<PluginRegistry>();
    }

    m_host = Host::get();
    if (m_host != nullptr && m_host->events() != nullptr) {
        connect(m_host->events(), &HostEvents::instanceRegistered, this,
                &AppController::onHostInstanceRegistered, qontrol::UNIQUE);
        connect(m_host->events(), &HostEvents::instanceRemoved, this,
                &AppController::onHostInstanceRemoved, qontrol::UNIQUE);
        connect(m_host->events(), &HostEvents::settingSectionsChanged, this,
                &AppController::onHostSettingsSectionsChanged, qontrol::UNIQUE);
    }

    connect(this, &AppController::regtestDefaultsReady, this,
            &AppController::onRegtestDefaultsReady, qontrol::UNIQUE);
    connect(m_plugin_registry.get(), &PluginRegistry::enabledPluginsChanged, this,
            &AppController::onPluginRegistryChanged, qontrol::UNIQUE);

    applyPersistedThemeAtStartup();
    listAccounts();

    auto *thread = QThread::create([this]() -> void {
        auto defaults = ::get_regtest_defaults();
        if (defaults.is_ok) {
            auto blindbit = QString::fromStdString(std::string(defaults.blindbit_url.c_str()));
            emit regtestDefaultsReady(blindbit);
        }
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

auto AppController::pluginIdForModule(const IModule *module) const -> QString {
    if (module == nullptr || m_plugin_registry == nullptr) {
        return {};
    }

    for (auto *plugin : m_plugin_registry->plugins()) {
        if (plugin == nullptr) {
            continue;
        }
        const auto modules = plugin->modules();
        bool ownsModule = std::ranges::find(modules, module) != modules.end();
        if (!ownsModule) {
            continue;
        }
        return plugin->meta().value(QStringLiteral("id")).trimmed();
    }

    return {};
}

auto AppController::moduleForPluginId(const QString &plugin_id) const -> IModule * {
    if (plugin_id.isEmpty() || m_plugin_registry == nullptr) {
        return nullptr;
    }

    const auto modules = m_plugin_registry->enabledModulesForLauncher();
    for (auto *module : modules) {
        if (module == nullptr) {
            continue;
        }
        if (pluginIdForModule(module) == plugin_id) {
            return module;
        }
    }

    return nullptr;
}

auto AppController::pluginIdForAccount(const QString &account_id) -> QString {
    auto config = ::config_from_file(rust::String(account_id.toStdString()));
    if (!config->is_ok()) {
        qWarning() << "Failed to load config for account" << account_id << ":"
                   << QString::fromStdString(std::string(config->get_error().c_str()));
        return {};
    }
    return QString::fromStdString(std::string(config->get_plugin_id().c_str()));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AppController::addAccount(const QString &name) -> void {
    if (isAccountOpen(name)) {
        qCritical() << "Account " << name << " already exists!";
        return;
    }

    QString pluginId = pluginIdForAccount(name);
    auto *module = moduleForPluginId(pluginId);
    if (module == nullptr) {
        qCritical() << "No enabled module found for plugin" << pluginId << "account" << name;
        return;
    }

    if (!m_plugin_registry->startLauncherInstance(module, name)) {
        qCritical() << "Failed to start account instance:" << name;
        return;
    }
    // m_open_accounts is populated by onHostInstanceRegistered when the instance
    // registers with the host, so no explicit insert here.
}

auto AppController::removeAccount(const QString &account) -> void {
    // removeInstance stops the instance and ignores an id the host does not hold.
    if (auto *hostImpl = dynamic_cast<HostImpl *>(m_host); hostImpl != nullptr) {
        hostImpl->removeInstance(account);
    }

    m_open_accounts.remove(account);
    listAccounts();
}

auto AppController::finalizeListAccounts() -> void {
    QList<QString> accounts;
    for (const auto &id : m_config_order) {
        if (m_discovered_accounts.contains(id)) {
            accounts.append(id);
        }
    }

    if (accounts.isEmpty()) {
        emit accountList(m_config_order);
        return;
    }
    emit accountList(accounts);
}

auto AppController::listAccounts() -> void {
    m_config_order.clear();
    m_known_configs.clear();
    m_discovered_accounts.clear();

    const auto raccounts = list_configs();
    for (auto acc : raccounts) {
        const auto account = QString::fromStdString(std::string(acc.c_str()));
        m_config_order.append(account);
        m_known_configs.insert(account);
    }

    if (m_plugin_registry == nullptr) {
        emit accountList(m_config_order);
        return;
    }

    // The registry already lists each enabled module's instances when the module
    // is activated, so read its collected results instead of issuing a second
    // module->list() round-trip. Late-arriving results refresh us through the
    // registry's enabledPluginsChanged signal (see onPluginRegistryChanged).
    const auto modules = m_plugin_registry->enabledModulesForLauncher();
    for (auto *module : modules) {
        if (module == nullptr) {
            continue;
        }
        for (const auto &instance : m_plugin_registry->launcherInstancesForModule(module)) {
            const auto &accountId = instance.first;
            if (m_known_configs.contains(accountId)) {
                m_discovered_accounts.insert(accountId);
            }
        }
    }

    finalizeListAccounts();
}

auto AppController::onPluginRegistryChanged() -> void {
    listAccounts();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AppController::createAccount(const QString &name, const QString &descriptor, Network network,
                                  const QString &blindbit_url, const QString &electrum_url)
    -> void {
    Q_UNUSED(name);
    Q_UNUSED(descriptor);
    Q_UNUSED(network);
    Q_UNUSED(blindbit_url);
    Q_UNUSED(electrum_url);
    qCritical()
        << "createAccount is deprecated in AppController and should be handled by module UI";
}

auto AppController::openAccount(const QString &name) -> void {
    addAccount(name);
    listAccounts();
}

// NOLINTNEXTLINE(readability-make-member-function-const)
auto AppController::deleteAccount(const QString &name) -> void {
    auto *modal = new modal::ConfirmDelete(name);
    connect(modal, &modal::ConfirmDelete::confirmed, this, &AppController::onDeleteConfirmed,
            qontrol::UNIQUE);
    AppController::execModal(modal);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto AppController::openAppSettings() -> void {
    AppController::execModal(new modal::AppSettings());
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void AppController::showKeysTab() {
    auto *window = dynamic_cast<MainWindow *>(AppController::window());
    if (window != nullptr) {
        window->showKeysTab();
    }
}

auto AppController::onDeleteConfirmed(const QString &account) -> void {
    if (isAccountOpen(account)) {
        removeAccount(account);
    }
    if (!delete_config(rust::String(account.toStdString()))) {
        qCritical() << "Failed to delete account:" << account;
    }
    listAccounts();
}

auto AppController::onHostInstanceRemoved(const QString &id) -> void {
    m_open_accounts.remove(id);
    applyPersistedThemeAtStartup();
    // The launcher dims open accounts: a closed one comes back to reopen.
    listAccounts();
}

auto AppController::onHostInstanceRegistered(const QString &id) -> void {
    m_open_accounts.insert(id);
    applyPersistedThemeAtStartup();
    listAccounts();
}

auto AppController::onHostSettingsSectionsChanged() -> void {
    if (m_plugin_registry == nullptr) {
        return;
    }
    m_plugin_registry->reloadEnabledPlugins();
}

auto AppController::accounts() -> int {
    return m_open_accounts.size();
}

auto AppController::isAccountOpen(const QString &name) const -> bool {
    return m_open_accounts.contains(name);
}

auto AppController::onRegtestDefaultsReady(const QString &blindbit) -> void {
    m_regtest_defaults = RegtestDefaultsInfo{.blindbit_url = blindbit};
}

auto AppController::regtestDefaults() const -> std::optional<RegtestDefaultsInfo> {
    return m_regtest_defaults;
}

auto AppController::applyPersistedThemeAtStartup() -> void {
    QString activeTheme = QString::fromStdString(std::string(app_active_theme().c_str())).trimmed();
    if (activeTheme.isEmpty()) {
        return;
    }

    m_startup_active_theme = activeTheme;

    auto *theme = Theme::get();
    if (theme == nullptr) {
        return;
    }

    if (activeTheme.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0) {
        theme->setMode(ThemeMode::Dark);
        m_startup_custom_theme_applied = true;
        return;
    }

    if (activeTheme.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0) {
        theme->setMode(ThemeMode::Light);
        m_startup_custom_theme_applied = true;
        return;
    }

    if (!m_startup_custom_theme_applied) {
        requestPersistedThemePalette();
    }
}

auto AppController::requestPersistedThemePalette() -> void {
    m_startup_theme_providers.clear();
    m_startup_pending_theme_list.clear();
    m_startup_pending_palette.clear();

    if (m_host == nullptr) {
        return;
    }

    auto *hostImpl = dynamic_cast<HostImpl *>(m_host);
    if (hostImpl == nullptr) {
        return;
    }
    const auto instances = hostImpl->liveInstances();
    for (auto *instance : instances) {
        if (instance == nullptr) {
            continue;
        }
        auto *provider = instance->theme();
        if (provider == nullptr || !provider->implemented()) {
            continue;
        }
        if (!m_startup_theme_providers.contains(provider)) {
            m_startup_theme_providers.append(provider);
            connect(provider, qOverload<ReqId, QStringList>(&IThemeProvider::themes), this,
                    &AppController::onStartupThemeNames, qontrol::UNIQUE);
            connect(provider, qOverload<ReqId, theme::Palette>(&IThemeProvider::palette), this,
                    &AppController::onStartupThemePalette, qontrol::UNIQUE);
        }
        ReqId reqId = provider->themes();
        if (reqId != 0) {
            m_startup_pending_theme_list.insert(reqId, provider);
        }
    }
}

auto AppController::onStartupThemeNames(ReqId req_id, const QStringList &themes) -> void {
    if (!m_startup_pending_theme_list.contains(req_id) || m_startup_custom_theme_applied ||
        m_startup_active_theme.isEmpty()) {
        return;
    }

    auto *provider = m_startup_pending_theme_list.take(req_id);
    if (provider == nullptr) {
        return;
    }

    bool hasTheme = std::ranges::any_of(themes, [this](const QString &name) {
        return name.trimmed().compare(m_startup_active_theme, Qt::CaseInsensitive) == 0;
    });
    if (!hasTheme) {
        return;
    }

    ReqId paletteReqId = provider->palette(m_startup_active_theme);
    if (paletteReqId != 0) {
        m_startup_pending_palette.insert(paletteReqId, provider);
    }
}

auto AppController::onStartupThemePalette(ReqId req_id, theme::Palette palette) -> void {
    if (!m_startup_pending_palette.contains(req_id) || m_startup_custom_theme_applied) {
        return;
    }
    m_startup_pending_palette.remove(req_id);

    auto *theme = Theme::get();
    if (theme == nullptr) {
        return;
    }

    theme->setPalette(palette);
    theme->apply();
    m_startup_custom_theme_applied = true;
}
