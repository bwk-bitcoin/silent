#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <optional>

#include <interfaces/types.h>

class QWidget;
class IInstance;
class IKeys;
class IModule;
class IStorage;
class ITransactions;

struct PriceSource {
    QString id;
    QString name;
    QStringList currencies;
};

struct SelectedPrice {
    QString source;
    QString currency;
    std::optional<double> rate;
};

enum class FeeMode { Manual, Default, Smart };
enum class FeeTier { Low, Medium, Fast };

struct FeePreferences {
    FeeMode mode = FeeMode::Manual;
    QString default_source;
    QString smart_source;
    FeeTier tier = FeeTier::Medium;
    auto operator==(const FeePreferences &) const -> bool = default;
};

struct FeeSource {
    QString id;
    QString name;
    bool has_default = false;
    bool has_smart = false;
};

struct SelectedFees {
    FeePreferences preferences;
    std::optional<double> default_rate;
    std::optional<double> low;
    std::optional<double> medium;
    std::optional<double> fast;
    auto operator==(const SelectedFees &) const -> bool = default;

    [[nodiscard]] auto rate(FeeMode mode, FeeTier tier) const -> std::optional<double> {
        switch (mode) {
        case FeeMode::Manual:
            return std::nullopt;
        case FeeMode::Default:
            return default_rate;
        case FeeMode::Smart:
            switch (tier) {
            case FeeTier::Low:
                return low;
            case FeeTier::Medium:
                return medium;
            case FeeTier::Fast:
                return fast;
            }
        }
        return std::nullopt;
    }
};

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
    void launcherRefreshRequested();
    void priceSourcesChanged();
    void selectedPriceChanged();
    void feeSourcesChanged();
    void selectedFeesChanged();
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
    // The live instances, as the calling module sees them: each one's account() is a
    // handle that stamps `caller` on every IAccount call, refuses it when the caller may
    // not use accounts (see requestAccess()), and passes on only the answers to the
    // caller's own requests. A refused request returns 0 and answers
    // error(std::nullopt, ...).
    [[nodiscard]] virtual QList<IInstance *> instances(IModule *caller) = 0;
    [[nodiscard]] virtual QList<plugin::SignerEntry> signers() const = 0;

    // The keys module registers itself once. A second registration from a different
    // module is refused, the way a duplicate instance id is. keys() answers null until
    // the module has registered, so every caller checks.
    virtual void setKeys(IKeys *keys, IModule *owner) = 0;
    // The keys, as the calling module sees them: a handle that stamps `caller` on every
    // request, refuses it with error() when the caller may not use keys, and passes on
    // only the answers to the requests it allocated with newRequest(). The same handle
    // for every call of one module.
    [[nodiscard]] virtual IKeys *keys(IModule *caller) = 0;
    // Account grant, no account ownership restriction. Finalization and network
    // submission do not mutate account transaction history or saved PSBTs.
    [[nodiscard]] virtual auto transactions(IModule *caller) -> ITransactions * = 0;
    // What the calling module may use. For an external plugin, asks the user for each
    // grant in `wanted` its library has no answer for yet, and saves the answer; a grant
    // already refused is not asked again (the user changes it in Settings). The host
    // also asks on a plugin's first IAccount or IKeys call, so asking here first is
    // only needed to ask at a moment the plugin picks. GUI thread only.
    virtual auto requestAccess(IModule *caller, plugin::Access wanted) -> plugin::Access = 0;

    // The calling module's storage (see interfaces/storage.h): the private storage of its
    // plugin, shared by the plugin's modules, each through its own IStorage. The same one
    // for every call of one module, bound to the caller the host knows, so no request
    // names who sends it. For an external plugin, asks the user for the Saved Data grant
    // when its library has no answer yet; every request checks the grant. Null when the
    // caller is no active module or the storage is unavailable. GUI thread only.
    [[nodiscard]] virtual auto storage(IModule *caller) -> IStorage * = 0;

    // A module states whether it currently needs signing devices polled. The host keeps
    // one flag per module and polls while any of them is true, so asking twice is the
    // same as asking once and there is nothing to balance.
    virtual void requestSignerPolling(IModule *requester, bool enabled) = 0;
    // Asks every enabled module to list its instances again, so the launcher and
    // account discovery see an account a module just wrote to disk.
    // Callable from any thread; the work runs on the GUI thread. It never starts an instance.
    virtual void refreshLauncher() = 0;

    virtual TabId openTab(QWidget *content, IInstance *owner) = 0;
    virtual void setTabTitle(TabId tab, const QString &title) = 0;
    // delete_content=false detaches the tab from view but keeps the content
    // widget alive, leaving its deletion to the caller.
    virtual void closeTab(TabId tab, bool delete_content = true) = 0;

    virtual SectionId addSettingSection(QWidget *content, IInstance *owner,
                                        const QString &title = QString()) = 0;
    virtual void delSettingSection(SectionId section) = 0;
    [[nodiscard]] virtual QList<SettingSectionView> settingSections() const = 0;

    virtual WizardId openWizard(QWidget *content, IInstance *owner) = 0;
    virtual void closeWizard(WizardId wizard) = 0;

    virtual HostI18n *i18n() = 0;
    virtual HostEvents *events() = 0;

    // GUI-thread cached reads, never plugin requests. Missing selections are retained.
    [[nodiscard]] virtual auto priceSources() const -> QList<PriceSource> = 0;
    [[nodiscard]] virtual auto selectedPrice() const -> SelectedPrice = 0;
    virtual void selectPrice(const QString &source, const QString &currency) = 0;
    [[nodiscard]] virtual auto feeSources() const -> QList<FeeSource> = 0;
    [[nodiscard]] virtual auto selectedFees() const -> SelectedFees = 0;
    virtual void selectFees(const FeePreferences &preferences) = 0;
};
