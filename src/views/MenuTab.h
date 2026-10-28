#pragma once

#include "host/PluginRegistry.h"
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

namespace catalog {
class Button;
}

#include <Qontrol>

class Host;
class HostEvents;
class IModule;

class MenuTab : public QWidget {
    Q_OBJECT

public:
    explicit MenuTab(QWidget *parent = nullptr);

signals:
    void createAccount();

public slots:
    void onAccountList(const QList<QString> &accounts);
    void onCreateClicked();
    void onItemClicked();
    void onTrashClicked();
    void onDeleteConfirmed(const QString &name);
    void onAccountDeletionFinished(IModule *module, const QString &id, const QString &error);
    void onHostInstanceChanged(const QString &id);
    void onHostSettingsChanged();
    void onPluginRegistryChanged();
    void onRefreshLauncherItems();

protected:
    void init();
    void doConnect();
    void view();
    void clearRows();
    void rebuildRows();

private:
    struct LauncherEntry {
        IModule *module = nullptr;
        QString id;
        QString name;
        bool canDelete = false;
    };

    catalog::Button *m_create_btn = nullptr;
    qontrol::Column *m_accounts_column = nullptr;
    QList<QWidget *> m_rows;
    QList<LauncherEntry> m_entries;
    // The built-in creators, then the external plugins' ones, as the account type modal
    // lists them.
    QList<PluginRegistry::CreatorLauncher> m_creators;
    QPointer<IModule> m_pending_delete_module;
    QString m_pending_delete_id;
    Host *m_host = nullptr;
    HostEvents *m_host_events = nullptr;
};
