#include "MenuTab.h"
#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/module.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "views/modals/AccountType.h"
#include "views/modals/ConfirmDelete.h"
#include <QGraphicsOpacityEffect>
#include <Qontrol>
#include <common.h>

using catalog::Label;
using catalog::LabelRole;

MenuTab::MenuTab(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
}

void MenuTab::init() {
    m_create_btn = catalog::btnCreateAccount();
    m_create_btn->setVisible(false);

    m_accounts_column = new qontrol::Column;

    m_host = Host::get();
    if (m_host != nullptr) {
        m_host_events = m_host->events();
    }
}

void MenuTab::doConnect() {
    auto *controller = AppController::get();
    connect(m_create_btn, &QPushButton::clicked, this, &MenuTab::onCreateClicked, qontrol::UNIQUE);
    connect(controller, &AppController::accountList, this, &MenuTab::onAccountList,
            qontrol::UNIQUE);
    if (controller != nullptr && controller->pluginRegistry() != nullptr) {
        connect(controller->pluginRegistry(), &PluginRegistry::enabledPluginsChanged, this,
                &MenuTab::onPluginRegistryChanged, qontrol::UNIQUE);
    }
    if (m_host_events != nullptr) {
        connect(m_host_events, &HostEvents::instanceRegistered, this,
                &MenuTab::onHostInstanceChanged, qontrol::UNIQUE);
        connect(m_host_events, &HostEvents::instanceRemoved, this, &MenuTab::onHostInstanceChanged,
                qontrol::UNIQUE);
        connect(m_host_events, &HostEvents::settingSectionsChanged, this,
                &MenuTab::onHostSettingsChanged, qontrol::UNIQUE);
    }
    onRefreshLauncherItems();
}

void MenuTab::onCreateClicked() {
    AppController::execModal(new modal::AccountType(m_creators));
}

void MenuTab::onItemClicked() {
    auto *widget = qobject_cast<QWidget *>(sender());
    if (widget == nullptr) {
        return;
    }
    auto id = widget->property("instanceId").toString();
    auto modulePtr = widget->property("modulePtr").toULongLong();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *module = reinterpret_cast<IModule *>(modulePtr);
    if (module == nullptr || id.isEmpty()) {
        return;
    }
    // Route through the registry so the started instance is attributed to its
    // owning plugin; a direct module->startInstance() leaves it unowned and
    // blocks the plugin from being disabled later.
    auto *controller = AppController::get();
    auto *registry = controller != nullptr ? controller->pluginRegistry() : nullptr;
    if (registry != nullptr) {
        registry->startLauncherInstance(module, id);
    } else {
        module->startInstance(id);
    }
}

void MenuTab::onTrashClicked() {
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (btn == nullptr) {
        return;
    }
    auto name = btn->property("accountName").toString();
    if (!btn->property("external").toBool()) {
        AppController::get()->deleteAccount(name);
        return;
    }

    auto modulePtr = btn->property("modulePtr").toULongLong();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *module = reinterpret_cast<IModule *>(modulePtr);
    if (module == nullptr || name.isEmpty()) {
        return;
    }
    m_pending_delete_module = module;
    m_pending_delete_id = name;
    auto *modal = new modal::ConfirmDelete(name);
    connect(modal, &modal::ConfirmDelete::confirmed, this, &MenuTab::onExternalDeleteConfirmed,
            qontrol::UNIQUE);
    AppController::execModal(modal);
}

void MenuTab::onExternalDeleteConfirmed(const QString &name) {
    auto *controller = AppController::get();
    auto *registry = controller != nullptr ? controller->pluginRegistry() : nullptr;
    if (registry != nullptr && m_pending_delete_module != nullptr && name == m_pending_delete_id) {
        registry->deleteInstance(m_pending_delete_module, name);
    }
    m_pending_delete_module = nullptr;
    m_pending_delete_id.clear();
}

void MenuTab::view() {
    auto *mark = new Label(LabelRole::Title);
    mark->setPixmap(icon::silentMark().pixmap(48, 48));
    mark->setAlignment(Qt::AlignCenter);
    auto *markRow = (new qontrol::Row)->pushSpacer()->push(mark)->pushSpacer();

    auto *title = new Label(TR("main-window-title"), LabelRole::Title);
    title->setAlignment(Qt::AlignCenter);

    auto *titleRow = (new qontrol::Row)->pushSpacer()->push(title)->pushSpacer();
    auto *createRow = (new qontrol::Row)->pushSpacer()->push(m_create_btn)->pushSpacer();

    auto *col = (new qontrol::Column)
                    ->pushSpacer()
                    ->push(markRow)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(titleRow)
                    ->pushSpacer(resolve(Spacing::XL))
                    ->push(m_accounts_column)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(createRow)
                    ->pushSpacer();

    setLayout(col->layout());
}

void MenuTab::clearRows() {
    catalog::clearRows(m_accounts_column);
    m_rows.clear();
}

void MenuTab::onAccountList(const QList<QString> &accounts) {
    m_sp_accounts.clear();
    for (const auto &account : accounts) {
        m_sp_accounts.insert(account);
    }
    onRefreshLauncherItems();
}

void MenuTab::ensureModuleConnections() {
    auto *controller = AppController::get();
    if (controller == nullptr || controller->pluginRegistry() == nullptr) {
        return;
    }

    auto modules = controller->pluginRegistry()->enabledModulesForLauncher();
    if (modules == m_modules) {
        return;
    }

    for (auto *module : m_modules) {
        if (module == nullptr) {
            continue;
        }
        disconnect(module, &IModule::instances, this, &MenuTab::onModuleInstances);
    }

    m_modules = modules;
    m_pending_list.clear();
    m_sync_list_responses.clear();
    m_items_by_module.clear();
    for (auto *module : m_modules) {
        if (module == nullptr) {
            continue;
        }
        connect(module, &IModule::instances, this, &MenuTab::onModuleInstances, qontrol::UNIQUE);
    }
}

void MenuTab::onRefreshLauncherItems() {
    ensureModuleConnections();
    for (auto *module : m_modules) {
        if (module == nullptr) {
            continue;
        }
        ReqId reqId = module->list();
        if (reqId == 0) {
            continue;
        }
        m_pending_list.insert(module, reqId);
        if (m_sync_list_responses.contains(module) &&
            m_sync_list_responses.value(module).first == reqId) {
            auto response = m_sync_list_responses.take(module);
            applyModuleInstances(module, response.first, response.second);
        }
    }
}

void MenuTab::onModuleInstances(ReqId req_id, QList<QPair<QString, QString>> instances) {
    auto *module = qobject_cast<IModule *>(sender());
    if (module == nullptr) {
        return;
    }
    applyModuleInstances(module, req_id, std::move(instances));
}

void MenuTab::applyModuleInstances(IModule *module, ReqId req_id,
                                   QList<QPair<QString, QString>> instances) {
    if (!m_pending_list.contains(module) || m_pending_list.value(module) != req_id) {
        if (req_id != 0) {
            m_sync_list_responses.insert(module, {req_id, std::move(instances)});
        }
        return;
    }

    m_pending_list.remove(module);
    m_items_by_module.insert(module, instances);
    rebuildRows();
}

void MenuTab::onHostInstanceChanged(const QString &id) {
    Q_UNUSED(id);
    AppController::get()->listAccounts();
    onRefreshLauncherItems();
}

void MenuTab::onHostSettingsChanged() {
    onRefreshLauncherItems();
}

void MenuTab::onPluginRegistryChanged() {
    onRefreshLauncherItems();
}

void MenuTab::rebuildRows() {
    clearRows();
    m_entries.clear();

    auto *controller = AppController::get();
    auto *registry = controller != nullptr ? controller->pluginRegistry() : nullptr;

    QList<LauncherEntry> openEntries;
    m_creators.clear();

    // Launcher module order, so the built-in creators keep the plugin order (sp first).
    for (auto *module : m_modules) {
        for (const auto &item : m_items_by_module.value(module)) {
            QString id = item.first.trimmed();
            bool isCreator = PluginRegistry::isCreatorInstanceId(id);
            // External-plugin creators are listed after the built-in ones, below.
            if (isCreator && registry != nullptr && !registry->isBuiltinModule(module)) {
                continue;
            }

            bool isBuiltin = registry != nullptr && registry->isBuiltinModule(module);

            LauncherEntry entry;
            entry.module = module;
            entry.id = id;
            entry.name = item.second.trimmed();
            if (entry.name.isEmpty()) {
                entry.name = entry.id;
            }
            entry.external = !isBuiltin;
            // Built-in (SP) accounts keep their existing delete path; every external
            // non-creator entry is a persisted instance the owning module can delete.
            entry.canDelete = isBuiltin ? m_sp_accounts.contains(entry.id) : !isCreator;

            if (isCreator) {
                m_creators.append({.module = module, .id = entry.id, .name = entry.name});
            } else {
                openEntries.append(entry);
            }
        }
    }

    m_entries = openEntries;
    for (const auto &entry : m_entries) {
        auto *btn = catalog::btnOpenWallet(entry.name);
        connect(btn, &QPushButton::clicked, this, &MenuTab::onItemClicked, qontrol::UNIQUE);
        QWidget *launcher = btn;
        launcher->setProperty("instanceId", entry.id);
        launcher->setProperty("modulePtr",
                              QVariant::fromValue(reinterpret_cast<qulonglong>(entry.module)));

        auto *row = (new qontrol::Row)->pushSpacer()->push(launcher);
        if (entry.canDelete) {
            auto *trashBtn = catalog::btnDeleteWallet();
            trashBtn->setProperty("accountName", entry.id);
            trashBtn->setProperty("external", entry.external);
            trashBtn->setProperty("modulePtr",
                                  QVariant::fromValue(reinterpret_cast<qulonglong>(entry.module)));
            connect(trashBtn, &QPushButton::clicked, this, &MenuTab::onTrashClicked,
                    qontrol::UNIQUE);
            row->pushSpacer(resolve(Spacing::XS))->push(trashBtn);
        }
        row->pushSpacer();

        if (AppController::get()->isAccountOpen(entry.id)) {
            row->setEnabled(false);
            auto *dim = new QGraphicsOpacityEffect(row);
            dim->setOpacity(0.5); // design dims already-open wallets
            row->setGraphicsEffect(dim);
        }

        if (!m_rows.isEmpty()) {
            m_accounts_column->pushSpacer(resolve(Spacing::S));
        }

        m_accounts_column->push(row);
        m_rows.append(row);
    }

    if (registry != nullptr) {
        m_creators.append(registry->externalCreatorLaunchers());
    }
    m_create_btn->setVisible(!m_creators.isEmpty());
}
