#pragma once

#include "host/CallerHandles.h"
#include "host/ServiceAssembly.h"
#include "interfaces/backend/provider.h"
#include "interfaces/host.h"
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <silent.h>
#include <utility>

namespace host {
class StorageHandle;
} // namespace host

class IKeys;
class MainWindow;
class PriceFeedCoordinator;
class QDialog;
class QEventLoop;
class QThread;
class QTimer;

namespace pluginaccess {
class Job;
}

class HostImpl final : public QObject, public Host {
    Q_OBJECT

public:
    HostImpl();
    ~HostImpl() override;

    void registerInstance(const QString &id, IInstance *instance, IModule *owner) override;
    void removeInstance(const QString &id, IModule *owner) override;
    void retireInstance(IInstance *self) override;
    [[nodiscard]] QList<IInstance *> instances(IModule *caller) override;
    [[nodiscard]] QList<plugin::SignerEntry> signers() const override;
    [[nodiscard]] auto priceSources() const -> QList<PriceSource> override;
    [[nodiscard]] auto selectedPrice() const -> SelectedPrice override;
    void selectPrice(const QString &source, const QString &currency) override;
    [[nodiscard]] auto feeSources() const -> QList<FeeSource> override;
    [[nodiscard]] auto selectedFees() const -> SelectedFees override;
    void selectFees(const FeePreferences &preferences) override;
    // The registry supplies the stable plugin id once it attributes a live instance.
    void registerPriceSource(const QString &instance_id, const QString &plugin_id);
    void setKeys(IKeys *keys, IModule *owner) override;
    [[nodiscard]] IKeys *keys(IModule *caller) override;
    [[nodiscard]] auto transactions(IModule *caller) -> ITransactions * override;
    auto requestAccess(IModule *caller, plugin::Access wanted) -> plugin::Access override;
    [[nodiscard]] auto storage(IModule *caller) -> IStorage * override;
    void requestSignerPolling(IModule *requester, bool enabled) override;
    void refreshLauncher() override;

    TabId openTab(QWidget *content, IInstance *owner,
                  PagePlacement placement = PagePlacement::Normal) override;
    void showInstanceTab(const QString &instance_id) override;
    void setTabTitle(TabId tab, const QString &title) override;
    void closeTab(TabId tab, bool delete_content = true) override;

    SectionId addSettingSection(QWidget *content, IInstance *owner,
                                const QString &title = QString()) override;
    void delSettingSection(SectionId section) override;

    WizardId openWizard(QWidget *content, IInstance *owner) override;
    void closeWizard(WizardId wizard) override;

    HostI18n *i18n() override;
    HostEvents *events() override;

    // The live instances themselves, for the host's own views: modules get handles.
    [[nodiscard]] auto liveInstances() const -> QList<IInstance *>;
    // The keys module itself, null until it registered.
    [[nodiscard]] auto keysModule() const -> IKeys *;
    // Who `module` is, as its handles stamp it. Empty fields for a module no active
    // plugin owns.
    [[nodiscard]] static auto caller(IModule *module) -> plugin::Caller;
    // Whether `module` may use `grant`. An external plugin with no answer yet is asked
    // when `ask` is set and the call is on the GUI thread.
    auto allowed(IModule *module, host::Grant grant, bool ask) -> bool;
    // Drops the handles a module that goes away was given.
    void dropCaller(IModule *module);
    void setCallerClosing(IModule *module, bool closing);
    void destroyClosedUi(IModule *module);
    [[nodiscard]] auto callerDrained(IModule *module) const -> bool;
    [[nodiscard]] auto callerFramesIdle(IModule *module) const -> bool;
    [[nodiscard]] auto callerDrainError(IModule *module) const -> QString;
    void enterFrame(IModule *module);
    void leaveFrame(IModule *module);
    // The services of one account UI, each from the source `request` names. Refused, with
    // nothing kept, when a provider is not an active module or does not supply its service.
    auto assembleServices(const host::AssemblyRequest &request) -> host::AssemblyResult;
    // No assembly holds a service `module` supplied.
    [[nodiscard]] auto servicesReleased(IModule *module) const -> bool;

    void onInstanceStopped(const QString &id);
    void onInstanceStopped(IInstance *instance);
    // Unrestricted teardown, kept off the Host interface so a plugin cannot drop
    // an instance it does not own. Stops the instance, then unregisters it.
    void removeInstance(const QString &id);
    [[nodiscard]] IModule *instanceOwner(const QString &id) const;
    [[nodiscard]] QList<SettingSectionView> settingSections() const override;
    // The module whose instance added the section.
    [[nodiscard]] auto settingSectionModule(SectionId section) const -> IModule *;

signals:
    // Emitted by the matching public method so the work always lands on the GUI
    // thread: auto-connection runs the slot directly for a caller already there,
    // and queues it otherwise. A module never has to marshal by hand.
    void registerInstanceRequested(QString id, IInstance *instance, IModule *owner);
    void removeInstanceRequested(QString id, IModule *owner);
    void retireInstanceRequested(IInstance *self);
    void setKeysRequested(IKeys *keys, IModule *owner);
    void signerPollingRequested(IModule *requester, bool enabled);
    void refreshLauncherRequested();
    void feePreferencesWritten(quint8 mode, const QString &source, const QString &error);

private slots:
    void onRegisterInstanceRequested(const QString &id, IInstance *instance, IModule *owner);
    void onRemoveInstanceRequested(const QString &id, IModule *owner);
    void onRetireInstanceRequested(IInstance *self);
    void onSetKeysRequested(IKeys *keys, IModule *owner);
    void onSignerPollingRequested(IModule *requester, bool enabled);
    void onRefreshLauncherRequested();
    void onHostedTabCloseRequested(QWidget *content);
    void onWizardModalFinished(int result);
    void onGrantRequestRead(uint64_t revision, const PluginAccessRequest &request);
    void onStorageOpened(uint64_t revision);
    void onGrantWaitExpired();
    void onAbandonGrantWaits();
    void onFeePreferencesWritten();
    void onDrainFeeWrites();

protected:
    void dispatchFeeWrite();
    void finishFeeWrite();
    [[nodiscard]] auto requestAccessChecked(IModule *caller, plugin::Access wanted)
        -> std::optional<plugin::Access>;
    // The answers stored, read on the calling thread: off the GUI thread nothing is asked.
    [[nodiscard]] static auto storedAccess(IModule *caller) -> std::optional<plugin::Access>;
    // Asks for `ask`, saved against `request`'s epoch, then reads a revisioned snapshot
    // of the published answers. None when the prompt or its save was abandoned.
    [[nodiscard]] auto askAccess(IModule *caller, const plugin::Caller &known,
                                 const PluginAccessRequest &request, plugin::Access ask)
        -> std::optional<PluginAccessRequest>;
    // Reads, or opens, off the GUI thread while a nested event loop waits for the answer:
    // none when the wait ended first, as `caller` started closing or the app quits.
    [[nodiscard]] auto readGrantRequest(IModule *caller, const plugin::Caller &known)
        -> std::optional<PluginAccessRequest>;
    [[nodiscard]] auto openStorageSession(IModule *caller, const plugin::Caller &known)
        -> std::optional<uint64_t>;
    void waitGrantJob(uint64_t job, IModule *caller, pluginaccess::Job *worker);
    void abandonGrantJob(uint64_t job);
    // Runs `modal`, a grant prompt or failure for `caller`, withdrawn when it starts
    // closing. The caller deletes it.
    void execAccessModal(IModule *caller, QDialog *modal);
    // `caller` is still the module `known` the registry attributed when its request
    // started at `revision`, and is not closing.
    [[nodiscard]] auto callerCurrent(IModule *caller, const plugin::Caller &known,
                                     uint64_t revision) const -> bool;
    void rememberClosedUi(IModule *module, QWidget *widget);
    // Asks `source` for the `kind` service of `assembly`. Empty on success.
    auto supplyService(host::ServiceAssembly &assembly, backend::ServiceKind kind,
                       const host::ServiceSource &source) -> QString;

private:
    struct TabEntry {
        TabId id = 0;
        QPointer<QWidget> content;
        IInstance *owner = nullptr;
        IModule *module = nullptr;
    };

    struct SectionEntry {
        SectionId id = 0;
        QPointer<QWidget> content;
        IInstance *owner = nullptr;
        QString title;
        IModule *module = nullptr;
    };

    struct GrantWait {
        QPointer<QEventLoop> loop;
        QPointer<QTimer> deadline;
        QPointer<pluginaccess::Job> worker;
        IModule *caller = nullptr;
    };

    struct WizardEntry {
        WizardId id = 0;
        QPointer<QWidget> content;
        QPointer<QWidget> modal;
        IInstance *owner = nullptr;
        IModule *module = nullptr;
    };

    [[nodiscard]] MainWindow *mainWindow() const;
    void dropInstanceHandles(IInstance *instance);
    void ensureMainWindowHooked();
    void closeOwnedUi(IInstance *owner);

    QHash<QString, IInstance *> m_instances;
    QHash<QString, IModule *> m_instance_owner;
    QPointer<IKeys> m_keys;
    QPointer<IModule> m_keys_owner;
    // The handles each module was given, so a module gets the same one every call.
    QHash<IModule *, host::KeysHandle *> m_keys_handles;
    QHash<IModule *, host::TransactionsHandle *> m_transaction_handles;
    QHash<IModule *, host::StorageHandle *> m_storage_handles;
    QHash<IModule *, QPointer<host::StorageHandle>> m_draining_storage;
    QHash<IModule *, QPointer<host::TransactionsHandle>> m_draining_transactions;
    QHash<IModule *, QPointer<host::KeysHandle>> m_draining_keys;
    QSet<IModule *> m_closing_callers;
    QHash<IModule *, int> m_active_calls;
    // Bumped each time a module starts closing: a grant its earlier request read or saved
    // is not handed out.
    QHash<IModule *, uint64_t> m_caller_revisions;
    QHash<IModule *, QList<QPointer<QWidget>>> m_closed_ui;
    // Every live assembly, so a supplier that starts closing retires the ones it serves.
    QList<QPointer<host::ServiceAssembly>> m_assemblies;
    std::map<std::pair<IModule *, IInstance *>, std::unique_ptr<host::InstanceHandle>>
        m_instance_handles;
    // The grant modal open, and the module it is for: while it is, a call made meanwhile
    // is refused, not asked again.
    QPointer<QDialog> m_access_modal;
    IModule *m_access_modal_caller = nullptr;
    // Kept until consumption; expiry removes both the wait and any delivered answer.
    uint64_t m_grant_job = 0;
    QHash<uint64_t, GrantWait> m_grant_waits;
    QHash<uint64_t, PluginAccessRequest> m_grant_requests;
    QHash<uint64_t, uint64_t> m_storage_sessions;
    bool m_grants_abandoned = false;
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
    PriceFeedCoordinator *m_prices = nullptr;
    QList<FeePreferences> m_fee_writes;
    QThread *m_fee_write_thread = nullptr;
    std::shared_ptr<QString> m_fee_write_error;
    bool m_fee_writes_stopped = false;
    QHash<QString, QString> m_price_source_ids;
    HostI18n *m_i18n = nullptr;
};
