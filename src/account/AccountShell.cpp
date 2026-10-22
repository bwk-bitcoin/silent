#include "account/AccountShell.h"

#include "catalog/Button.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QAbstractButton>
#include <QButtonGroup>
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
    menu->setAutoFillBackground(true);
    auto menuPal = menu->palette();
    menuPal.setColor(QPalette::Window, Theme::get()->palette().bgSecondary);
    menu->setPalette(menuPal);
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
    auto *column = (new qontrol::Column)->push(content)->push(status);
    column->layout()->setContentsMargins(0, 0, 0, 0);
    column->layout()->setSpacing(0);
    m_widget->setLayout(column->layout());
}

auto AccountShell::widget() const -> QWidget * {
    return m_widget;
}

void AccountShell::loadPanel(qontrol::Panel *panel) {
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

void AccountShell::onMenuClicked(int id) {
    // A request for an unavailable panel must not change the displayed selection.
    if (m_current_panel != nullptr) {
        button(QLatin1String(m_current_panel->name()))->setChecked(true);
    }
    emit panelRequested(m_entries[id].name);
}

} // namespace account
