#pragma once

#include "account/PanelCollection.h"
#include "account/Services.h"
#include "host/ServiceAssembly.h"

#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <memory>
#include <optional>

class IModule;
class QAbstractButton;
class QButtonGroup;
class QWidget;

namespace qontrol {
class Panel;
class Screen;
} // namespace qontrol

namespace account {

class SendSigning;

namespace view {
class Send;
} // namespace view

struct MenuEntry {
    QString name;
    QString label;
    QIcon icon;
    bool bottom = false;
};

auto accountMenuEntries() -> QList<MenuEntry>;

// The account page every account kind shares: the menu, the status bar, and the panels
// over the account's services. The module plugs in what is its own: its status details,
// its menu entries and their panels, Send's signing step and what Settings shows under the
// endpoints. The services may each come from another provider: the shell keeps only the
// assembly, never their owner. Not a plugin interface.
class AccountShell : public QObject {
    Q_OBJECT

public:
    // `status` is the module's status details, shown in the status bar.
    AccountShell(QList<MenuEntry> entries, QWidget *status, QWidget *parent);
    ~AccountShell() override;
    AccountShell(const AccountShell &) = delete;
    auto operator=(const AccountShell &) -> AccountShell & = delete;
    AccountShell(AccountShell &&) = delete;
    auto operator=(AccountShell &&) -> AccountShell & = delete;

    [[nodiscard]] auto widget() const -> QWidget *;
    // Assembles the services `request` names, each from its own source, once. False when
    // the host refuses them, or once a supplier retired them.
    auto assemble(const host::AssemblyRequest &request) -> bool;
    // The assembled services, while assemble() answers true.
    [[nodiscard]] auto services() const -> Services;
    // Drops the panels, then hands the services back: the next assemble() asks for them
    // again, even once a supplier retired them.
    void release();
    // Settings over the services, once, `extras` under the endpoints. See view::Settings
    // for `lock_connected`.
    void openSettings(QWidget *extras, bool lock_connected);
    // History, Coins, Receive and Send over the services, once: Send signs with `signing`,
    // Receive asks the keys as `caller`. The Send built.
    auto openFunds(SendSigning *signing, IModule *caller) -> view::Send *;
    [[nodiscard]] auto fundsOpen() const -> bool;
    // A panel of the module's own, named as its menu entry.
    void insertPanel(qontrol::Panel *panel);
    [[nodiscard]] auto screen(const QString &name) const -> std::optional<qontrol::Screen *>;
    // Shows the panel `name`, at once or as soon as it is built.
    void showPanel(const QString &name);

public slots:
    void onMenuClicked(int id);
    // A supplier of the services stops: the panels go, then the assembly hands the
    // services back.
    void onServicesRetired();

protected:
    [[nodiscard]] auto button(const QString &name) const -> QAbstractButton *;
    void display(qontrol::Panel *panel);
    // The module's status details in the shared bar: a top separator, then `status`.
    [[nodiscard]] static auto statusBar(QWidget *status) -> QWidget *;

private:
    QList<MenuEntry> m_entries;
    QWidget *m_widget = nullptr;
    QWidget *m_screen_container = nullptr;
    QButtonGroup *m_buttons = nullptr;
    QPointer<qontrol::Panel> m_current_panel;
    QString m_requested_panel = QStringLiteral("history");
    // Declared before the panels, which use it: they go first.
    std::unique_ptr<host::ServiceAssembly> m_assembly;
    bool m_retired = false;
    PanelCollection m_panels{this};
};

} // namespace account
