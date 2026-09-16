#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <interfaces/types.h>

class QWidget;
class IInstance;
class IKeys;
class IModule;

class HostI18n {
public:
    virtual ~HostI18n() = default;
    virtual bool registerPluginTranslations(const QString &plugin_id, const QString &dir) = 0;
    virtual void unregisterPluginTranslations(const QString &plugin_id) = 0;
};

class HostEvents : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

signals:
    void instanceRegistered(QString id);
    void instanceRemoved(QString id);
    void settingSectionsChanged();
    // The global "signers must poll" flag, emitted only when it flips.
    void signerPollingChanged(bool enabled);
};

class Host {
public:
    using TabId = quint64;
    using SectionId = quint64;
    using WizardId = quint64;
    struct SettingSectionView {
        SectionId id = 0;
        QWidget *content = nullptr;
        QString title;
    };

    virtual ~Host() = default;

    static Host *get();

    virtual void registerInstance(const QString &id, IInstance *instance, IModule *owner) = 0;
    // Drops one of the calling module's own instances; ignored for an id it did not register.
    virtual void removeInstance(const QString &id, IModule *owner) = 0;
    // An instance retires itself: it holds the device handle, so it sees the disconnect.
    virtual void retireInstance(IInstance *self) = 0;
    [[nodiscard]] virtual QList<IInstance*> instances() const = 0;
    [[nodiscard]] virtual QList<plugin::SignerEntry> signers() const = 0;

    // The keys module registers itself once. A second registration from a different
    // module is refused, the way a duplicate instance id is. keys() answers null until
    // the module has registered, so every caller checks.
    virtual void setKeys(IKeys *keys, IModule *owner) = 0;
    [[nodiscard]] virtual IKeys *keys() const = 0;

    // A module states whether it currently needs signing devices polled. The host keeps
    // one flag per module and polls while any of them is true, so asking twice is the
    // same as asking once and there is nothing to balance.
    virtual void requestSignerPolling(IModule *requester, bool enabled) = 0;

    virtual TabId openTab(QWidget *content, IInstance *owner) = 0;
    virtual void setTabTitle(TabId tab, const QString &title) = 0;
    // delete_content=false detaches the tab from view but keeps the content
    // widget alive, leaving its deletion to the caller.
    virtual void closeTab(TabId tab, bool delete_content = true) = 0;

    virtual SectionId addSettingSection(
        QWidget *content, IInstance *owner, const QString &title = QString()) = 0;
    virtual void delSettingSection(SectionId section) = 0;
    [[nodiscard]] virtual QList<SettingSectionView> settingSections() const = 0;

    virtual WizardId openWizard(QWidget *content, IInstance *owner) = 0;
    virtual void closeWizard(WizardId wizard) = 0;

    virtual HostI18n *i18n() = 0;
    virtual HostEvents *events() = 0;
};
