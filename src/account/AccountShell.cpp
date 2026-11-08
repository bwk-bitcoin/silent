#include "account/AccountShell.h"

#include "account/SendSigning.h"
#include "account/views/Coins.h"
#include "account/views/History.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "account/views/Settings.h"
#include "catalog/Button.h"
#include "catalog/containers/Separator.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDebug>
#include <QSize>
#include <QSizePolicy>
#include <QWidget>
#include <Qontrol>
#include <common.h>
#include <utility>

namespace account {

auto accountMenuEntries() -> QList<MenuEntry> {
    return {{.name = "history", .label = TR("musig-menu-history"), .icon = icon::history()},
            {.name = "coins", .label = TR("musig-menu-coins"), .icon = icon::coins()},
            {.name = "send", .label = TR("musig-menu-send"), .icon = icon::send()},
            {.name = "receive", .label = TR("musig-menu-receive"), .icon = icon::receive()},
            {.name = "settings",
             .label = TR("musig-menu-settings"),
             .icon = icon::settings(),
             .bottom = true}};
}

AccountShell::AccountShell(QList<MenuEntry> entries, QWidget *status, QWidget *parent)
    : QObject(parent),
      m_entries(std::move(entries)),
      m_widget(new QWidget(parent)) {
    auto *menu = new qontrol::Column;
    menu->setFixedWidth(metric::SIDEBAR_WIDTH);
    // Styled by the theme's stylesheet, which a theme switch rebuilds: a palette set here
    // would keep the color of the theme the page was built under.
    menu->setProperty("class", "sidebar");
    menu->setAttribute(Qt::WA_StyledBackground, true);
    int padding = resolve(Padding::XS);
    menu->layout()->setContentsMargins(padding, padding, padding, padding);

    m_buttons = new QButtonGroup(this);
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        const auto &entry = m_entries[i];
        if (entry.bottom) {
            menu->pushSpacer();
        } else if (i != 0) {
            menu->pushSpacer(resolve(Spacing::S));
        }
        auto *button =
            new catalog::Button(QStringLiteral("  ") + entry.label, catalog::ButtonRole::Menu);
        button->setIcon(entry.icon);
        constexpr int c_menu_icon_size = 20;
        button->setIconSize(QSize(c_menu_icon_size, c_menu_icon_size));
        button->setCheckable(true);
        button->setChecked(i == 0);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_buttons->addButton(button, static_cast<int>(i));
        menu->push(button);
    }
    connect(m_buttons, &QButtonGroup::idClicked, this, &AccountShell::onMenuClicked,
            qontrol::UNIQUE);

    auto *container = new qontrol::Column;
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    container->layout()->setContentsMargins(0, 0, 0, 0);
    m_screen_container = container;
    auto *content = (new qontrol::Row)->push(menu)->push(container);
    content->layout()->setContentsMargins(0, 0, 0, 0);
    content->layout()->setSpacing(0);
    auto *column = (new qontrol::Column)->push(content)->push(statusBar(status));
    column->layout()->setContentsMargins(0, 0, 0, 0);
    column->layout()->setSpacing(0);
    m_widget->setLayout(column->layout());
}

AccountShell::~AccountShell() {
    // The panels use the services: they go before the assembly hands them back.
    m_panels.clear();
}

auto AccountShell::widget() const -> QWidget * {
    return m_widget;
}

auto AccountShell::assemble(const host::AssemblyRequest &request) -> bool {
    if (m_assembly != nullptr) {
        return true;
    }
    if (m_retired) {
        return false;
    }
    auto result = assembleServices(request);
    if (result.assembly == nullptr) {
        qWarning() << "AccountShell: account services refused:" << result.error;
        return false;
    }
    m_assembly = std::move(result.assembly);
    connect(m_assembly.get(), &host::ServiceAssembly::retired, this,
            &AccountShell::onServicesRetired, qontrol::UNIQUE);
    return true;
}

auto AccountShell::services() const -> Services {
    return m_assembly == nullptr ? Services{} : servicesOf(*m_assembly);
}

void AccountShell::openSettings(QWidget *extras, bool lock_connected) {
    if (m_assembly == nullptr || m_panels.contains(QStringLiteral("settings"))) {
        delete extras;
        return;
    }
    insertPanel(
        new qontrol::Panel(new view::Settings(services(), extras, lock_connected), "settings"));
}

auto AccountShell::openFunds(SendSigning *signing, IModule *caller) -> view::Send * {
    if (m_assembly == nullptr || fundsOpen()) {
        delete signing;
        return nullptr;
    }
    auto shared = services();
    insertPanel(new qontrol::Panel(new view::History(shared), "history"));
    insertPanel(new qontrol::Panel(new view::Coins(shared), "coins"));
    insertPanel(new qontrol::Panel(new view::Receive(shared, caller), "receive"));
    auto *send = new view::Send(shared, signing);
    insertPanel(new qontrol::Panel(send, "send"));
    return send;
}

auto AccountShell::fundsOpen() const -> bool {
    return m_panels.contains(QStringLiteral("history"));
}

void AccountShell::insertPanel(qontrol::Panel *panel) {
    m_panels.insert(panel);
    if (panel->name() == m_requested_panel) {
        display(panel);
    }
}

auto AccountShell::screen(const QString &name) const -> std::optional<qontrol::Screen *> {
    return m_panels.screen(name);
}

void AccountShell::showPanel(const QString &name) {
    m_requested_panel = name;
    display(m_panels.value(name));
}

auto AccountShell::statusBar(QWidget *status) -> QWidget * {
    auto *bar = new QWidget;
    bar->setFixedHeight(metric::STATUS_BAR_HEIGHT);
    auto *separator = new catalog::Separator(catalog::Separator::Role::Horizontal, bar);
    auto *content = (new qontrol::Row)->push(status);
    content->layout()->setContentsMargins(resolve(Padding::S), resolve(Padding::XXS),
                                          resolve(Padding::S), resolve(Padding::XXS));
    content->layout()->setSpacing(0);
    auto *column = (new qontrol::Column)->push(separator)->push(content);
    column->layout()->setContentsMargins(0, 0, 0, 0);
    column->layout()->setSpacing(0);
    bar->setLayout(column->layout());
    return bar;
}

void AccountShell::display(qontrol::Panel *panel) {
    if (panel == nullptr || panel->widget() == nullptr) {
        return;
    }
    auto *selected = button(QLatin1String(panel->name()));
    if (selected == nullptr) {
        return;
    }
    if (m_current_panel != nullptr && m_current_panel->widget() != nullptr) {
        auto *current = m_current_panel->widget();
        m_screen_container->layout()->removeWidget(current);
        current->hide();
    }
    m_current_panel = panel;
    selected->setChecked(true);
    auto *screen = panel->widget();
    m_screen_container->layout()->addWidget(screen);
    screen->show();
}

auto AccountShell::button(const QString &name) const -> QAbstractButton * {
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].name == name) {
            return m_buttons->button(static_cast<int>(i));
        }
    }
    return nullptr;
}

// ===== Slots =====

void AccountShell::onMenuClicked(int id) {
    // A request for an unavailable panel must not change the displayed selection.
    if (m_current_panel != nullptr) {
        button(QLatin1String(m_current_panel->name()))->setChecked(true);
    }
    showPanel(m_entries[id].name);
}

void AccountShell::onServicesRetired() {
    m_retired = true;
    m_panels.clear();
    // The assembly is the sender: it goes once its signal returned.
    m_assembly.release()->deleteLater();
}

} // namespace account
