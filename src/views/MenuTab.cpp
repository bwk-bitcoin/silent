#include "MenuTab.h"
#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/MessageModal.h"
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
        connect(controller->pluginRegistry(), &PluginRegistry::accountDeletionFinished, this,
                &MenuTab::onAccountDeletionFinished, qontrol::UNIQUE);
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
    auto modulePtr = btn->property("modulePtr").toULongLong();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *module = reinterpret_cast<IModule *>(modulePtr);
    if (module == nullptr || name.isEmpty()) {
        return;
    }
    m_pending_delete_module = module;
    m_pending_delete_id = name;
    auto *modal = new modal::ConfirmDelete(name);
    connect(modal, &modal::ConfirmDelete::confirmed, this, &MenuTab::onDeleteConfirmed,
            qontrol::UNIQUE);
    AppController::execModal(modal);
}

void MenuTab::onDeleteConfirmed(const QString &name) {
    auto *controller = AppController::get();
    auto *registry = controller != nullptr ? controller->pluginRegistry() : nullptr;
    if (registry != nullptr && name == m_pending_delete_id) {
        registry->deleteInstance(m_pending_delete_module, name);
    }
    m_pending_delete_module = nullptr;
    m_pending_delete_id.clear();
}

void MenuTab::onAccountDeletionFinished(IModule *module, const QString &id, const QString &error) {
    Q_UNUSED(module);
    if (!error.isEmpty()) {
        rebuildRows();
        AppController::execModal(new catalog::MessageModal(
            TR("common-error"),
            TR("delete-wallet-title") + QStringLiteral(": ") + id + QLatin1Char('\n') + error));
    }
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

void MenuTab::onAccountList([[maybe_unused]] const QList<QString> &accounts) {
    onRefreshLauncherItems();
}

// Renders the instances the registry last got from each module. The registry asks the
// modules for them itself (on activation, account creation and deletion) and emits
// enabledPluginsChanged with each answer, so a refresh here never calls list(): that
// reads the module's storage on the GUI thread.
void MenuTab::onRefreshLauncherItems() {
    rebuildRows();
}

void MenuTab::onHostInstanceChanged(const QString &id) {
    if (AppController::get()->pluginRegistry()->accountDeletionPending(id)) {
        return;
    }
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

    const auto modules =
        registry != nullptr ? registry->enabledModulesForLauncher() : QList<IModule *>{};
    for (auto *module : modules) {
        for (const auto &item : registry->launcherInstancesForModule(module)) {
            QString id = item.first.trimmed();
            // The account types are listed by the Account Type modal.
            if (PluginRegistry::isCreatorInstanceId(id)) {
                continue;
            }

            LauncherEntry entry;
            entry.module = module;
            entry.id = id;
            entry.name = item.second.trimmed();
            if (entry.name.isEmpty()) {
                entry.name = entry.id;
            }
            m_entries.append(entry);
        }
    }

    for (const auto &entry : m_entries) {
        auto *btn = catalog::btnOpenWallet(entry.name);
        connect(btn, &QPushButton::clicked, this, &MenuTab::onItemClicked, qontrol::UNIQUE);
        QWidget *launcher = btn;
        launcher->setProperty("instanceId", entry.id);
        launcher->setProperty("modulePtr",
                              QVariant::fromValue(reinterpret_cast<qulonglong>(entry.module)));

        auto *trashBtn = catalog::btnDeleteWallet();
        trashBtn->setProperty("accountName", entry.id);
        trashBtn->setProperty("modulePtr",
                              QVariant::fromValue(reinterpret_cast<qulonglong>(entry.module)));
        connect(trashBtn, &QPushButton::clicked, this, &MenuTab::onTrashClicked, qontrol::UNIQUE);
        auto *row = (new qontrol::Row)
                        ->pushSpacer()
                        ->push(launcher)
                        ->pushSpacer(resolve(Spacing::XS))
                        ->push(trashBtn)
                        ->pushSpacer();

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

    m_creators = registry != nullptr ? registry->creatorLaunchers()
                                     : QList<PluginRegistry::CreatorLauncher>{};
    m_create_btn->setVisible(!m_creators.isEmpty());
}
