#pragma once

#include "interfaces/host.h"
#include <QObject>
#include <QHash>
#include <QPointer>

class IKeys;
class MainWindow;

class HostImpl final : public QObject, public Host {
    Q_OBJECT

public:
    HostImpl();

    void registerInstance(const QString &id, IInstance *instance, IModule *owner) override;
    void removeInstance(const QString &id, IModule *owner) override;
    void retireInstance(IInstance *self) override;
    [[nodiscard]] QList<IInstance *> instances() const override;
    [[nodiscard]] QList<plugin::SignerEntry> signers() const override;
    void setKeys(IKeys *keys, IModule *owner) override;
    [[nodiscard]] IKeys *keys() const override;
    void requestSignerPolling(IModule *requester, bool enabled) override;

    TabId openTab(QWidget *content, IInstance *owner) override;
    void setTabTitle(TabId tab, const QString &title) override;
    void closeTab(TabId tab, bool delete_content = true) override;

    SectionId addSettingSection(
        QWidget *content, IInstance *owner, const QString &title = QString()) override;
    void delSettingSection(SectionId section) override;

    WizardId openWizard(QWidget *content, IInstance *owner) override;
    void closeWizard(WizardId wizard) override;

    HostI18n *i18n() override;
    HostEvents *events() override;

    void onInstanceStopped(const QString &id);
    void onInstanceStopped(IInstance *instance);
    // Unrestricted teardown, kept off the Host interface so a plugin cannot drop
    // an instance it does not own. Stops the instance, then unregisters it.
    void removeInstance(const QString &id);
    [[nodiscard]] IModule *instanceOwner(const QString &id) const;
    [[nodiscard]] QList<SettingSectionView> settingSections() const override;

signals:
    // Emitted by the matching public method so the work always lands on the GUI
    // thread: auto-connection runs the slot directly for a caller already there,
    // and queues it otherwise. A module never has to marshal by hand.
    void registerInstanceRequested(QString id, IInstance *instance, IModule *owner);
    void removeInstanceRequested(QString id, IModule *owner);
    void retireInstanceRequested(IInstance *self);
    void setKeysRequested(IKeys *keys, IModule *owner);
    void signerPollingRequested(IModule *requester, bool enabled);

private slots:
    void onRegisterInstanceRequested(const QString &id, IInstance *instance, IModule *owner);
    void onRemoveInstanceRequested(const QString &id, IModule *owner);
    void onRetireInstanceRequested(IInstance *self);
    void onSetKeysRequested(IKeys *keys, IModule *owner);
    void onSignerPollingRequested(IModule *requester, bool enabled);
    void onHostedTabCloseRequested(QWidget *content);
    void onWizardModalFinished(int result);

private:
    struct TabEntry {
        TabId id = 0;
        QPointer<QWidget> content;
        IInstance *owner = nullptr;
    };
    struct SectionEntry {
        SectionId id = 0;
        QPointer<QWidget> content;
        IInstance *owner = nullptr;
        QString title;
    };
    struct WizardEntry {
        WizardId id = 0;
        QPointer<QWidget> content;
        QPointer<QWidget> modal;
        IInstance *owner = nullptr;
    };

    [[nodiscard]] MainWindow *mainWindow() const;
    void ensureMainWindowHooked();
    void closeOwnedUi(IInstance *owner);

    QHash<QString, IInstance *> m_instances;
    QHash<QString, IModule *> m_instance_owner;
    IKeys *m_keys = nullptr;
    IModule *m_keys_owner = nullptr;
    QHash<IModule *, bool> m_signer_polling_requests;
    bool m_signer_polling = false;
    QHash<TabId, TabEntry> m_tabs;
    QHash<IInstance *, TabId> m_tab_by_owner;
    QHash<SectionId, SectionEntry> m_sections;
    QHash<WizardId, WizardEntry> m_wizards;
    QHash<QWidget *, WizardId> m_wizard_by_modal;
    TabId m_next_tab_id = 0;
    SectionId m_next_section_id = 0;
    WizardId m_next_wizard_id = 0;
    bool m_main_window_hooked = false;
    HostEvents m_events;
    HostI18n *m_i18n = nullptr;
};
