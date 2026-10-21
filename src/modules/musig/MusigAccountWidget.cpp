#include "modules/musig/MusigAccountWidget.h"

#include "catalog/Button.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigStatusBar.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QButtonGroup>
#include <QSize>
#include <QSizePolicy>
#include <Qontrol>
#include <common.h>

using catalog::Button;
using catalog::ButtonRole;

namespace {
// The side menu icon size of sp's account tab.
constexpr int K_MENU_ICON_SIZE = 20;

auto menuButton(const QString &text, const QIcon &icon) -> Button * {
    // The leading spaces are the gap between the icon and the text, as in sp's menu.
    auto *btn = new Button(QStringLiteral("  ") + text, ButtonRole::Menu);
    btn->setIcon(icon);
    btn->setIconSize(QSize(K_MENU_ICON_SIZE, K_MENU_ICON_SIZE));
    btn->setCheckable(true);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return btn;
}
} // namespace

MusigAccountWidget::MusigAccountWidget(const QString &account, QWidget *parent) : QWidget(parent) {
    m_controller = new MusigAccountController(account, this);
    initUI();
    m_controller->loadPanels();
    m_controller->loadPanel("history");
    // The views show the stored coins before the first notification.
    m_controller->pollCoins();
    // Endpoint replacement takes the account away while the worker runs.
    if (!m_controller->electrumUrl().isEmpty()) {
        m_controller->startElectrum();
    }
}

MusigAccountWidget::~MusigAccountWidget() = default;

auto MusigAccountWidget::controller() -> MusigAccountController * {
    return m_controller;
}

void MusigAccountWidget::loadPanel(qontrol::Panel *panel) {
    if (panel == nullptr) {
        return;
    }

    if (m_current_panel != nullptr) {
        auto *currentWidget = m_current_panel->widget();
        if (currentWidget != nullptr) {
            m_screen_container->layout()->removeWidget(currentWidget);
            currentWidget->setVisible(false);
        }
    }

    m_current_panel = panel;
    for (auto it = m_menu_buttons.begin(); it != m_menu_buttons.end(); ++it) {
        if (m_controller->screen(it.key()).value_or(nullptr) == panel->screen()) {
            it.value()->setChecked(true);
            break;
        }
    }
    auto *widget = panel->widget();
    if (widget != nullptr) {
        m_screen_container->layout()->addWidget(widget);
        widget->setVisible(true);
    }
}

void MusigAccountWidget::initUI() {
    m_menu = new qontrol::Column(this);
    m_menu->setFixedWidth(metric::SIDEBAR_WIDTH);
    m_menu->setAutoFillBackground(true);
    auto menuPal = m_menu->palette();
    menuPal.setColor(QPalette::Window, Theme::get()->palette().bgSecondary);
    m_menu->setPalette(menuPal);
    int p = resolve(Padding::XS);
    m_menu->layout()->setContentsMargins(p, p, p, p);

    auto *historyBtn = menuButton(TR("musig-menu-history"), icon::history());
    historyBtn->setChecked(true);
    auto *coinsBtn = menuButton(TR("musig-menu-coins"), icon::coins());
    auto *sendBtn = menuButton(TR("musig-menu-send"), icon::send());
    auto *recvBtn = menuButton(TR("musig-menu-receive"), icon::receive());
    auto *psbtBtn = menuButton(TR("musig-menu-psbt"), icon::history());
    auto *settingsBtn = menuButton(TR("musig-menu-settings"), icon::settings());

    auto *btnGroup = new QButtonGroup(this);
    btnGroup->setExclusive(true);
    btnGroup->addButton(historyBtn);
    btnGroup->addButton(coinsBtn);
    btnGroup->addButton(sendBtn);
    btnGroup->addButton(recvBtn);
    btnGroup->addButton(psbtBtn);
    btnGroup->addButton(settingsBtn);
    m_menu_buttons = {{"history", historyBtn}, {"coins", coinsBtn}, {"send", sendBtn},
                      {"receive", recvBtn},    {"psbt", psbtBtn},   {"settings", settingsBtn}};

    m_menu->push(historyBtn)
        ->pushSpacer(resolve(Spacing::S))
        ->push(coinsBtn)
        ->pushSpacer(resolve(Spacing::S))
        ->push(sendBtn)
        ->pushSpacer(resolve(Spacing::S))
        ->push(recvBtn)
        ->pushSpacer(resolve(Spacing::S))
        ->push(psbtBtn)
        ->pushSpacer()
        ->push(settingsBtn);

    connect(historyBtn, &QPushButton::clicked, m_controller,
            &MusigAccountController::historyClicked, qontrol::UNIQUE);
    connect(coinsBtn, &QPushButton::clicked, m_controller, &MusigAccountController::coinsClicked,
            qontrol::UNIQUE);
    connect(sendBtn, &QPushButton::clicked, m_controller, &MusigAccountController::sendClicked,
            qontrol::UNIQUE);
    connect(recvBtn, &QPushButton::clicked, m_controller, &MusigAccountController::receiveClicked,
            qontrol::UNIQUE);
    connect(settingsBtn, &QPushButton::clicked, m_controller,
            &MusigAccountController::settingsClicked, qontrol::UNIQUE);
    connect(psbtBtn, &QPushButton::clicked, m_controller, &MusigAccountController::psbtClicked,
            qontrol::UNIQUE);

    auto *container = new qontrol::Column;
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    container->layout()->setContentsMargins(0, 0, 0, 0);
    m_screen_container = container;

    auto *contentWidget = (new qontrol::Row)->push(m_menu)->push(m_screen_container);
    contentWidget->layout()->setContentsMargins(0, 0, 0, 0);
    contentWidget->layout()->setSpacing(0);

    m_status_bar = new MusigStatusBar(m_controller, this);

    auto *mainCol = (new qontrol::Column)->push(contentWidget)->push(m_status_bar);
    mainCol->layout()->setContentsMargins(0, 0, 0, 0);
    mainCol->layout()->setSpacing(0);
    setLayout(mainCol->layout());
}
