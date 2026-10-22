#include "AccountWidget.h"
#include "AccountController.h"
#ifdef SILENT_MOCK_UI
#include "MockAccountController.h"
#endif
#include "StatusBar.h"
#include "catalog/Button.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "theme/Theme.h"
#include <QButtonGroup>
#include <Qontrol>
#include <common.h>
#include <qsizepolicy.h>

using catalog::Button;
using catalog::ButtonRole;

AccountWidget::AccountWidget([[maybe_unused]] const QString &account, QWidget *parent)
    : QWidget(parent) {
#ifdef SILENT_MOCK_UI
    m_controller = new MockAccountController(this);
#else
    m_controller = new AccountController(account, this);
#endif
    initUI();
#ifdef SILENT_MOCK_UI
    m_controller->loadPanels();
    m_controller->loadPanel("history");
#endif
}

void AccountWidget::initUI() {
    // Create side menu
    m_menu = new qontrol::Column(this);
    m_menu->setFixedWidth(metric::SIDEBAR_WIDTH);
    m_menu->setAutoFillBackground(true);
    auto menuPal = m_menu->palette();
    menuPal.setColor(QPalette::Window, Theme::get()->palette().bgSecondary);
    m_menu->setPalette(menuPal);
    int p = resolve(Padding::XS);
    m_menu->layout()->setContentsMargins(p, p, p, p);

    auto *historyBtn = new Button("  History", ButtonRole::Menu);
    historyBtn->setIcon(icon::history());
    historyBtn->setIconSize(QSize(20, 20));
    historyBtn->setCheckable(true);
    historyBtn->setChecked(true);
    historyBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *coinsBtn = new Button("  Coins", ButtonRole::Menu);
    coinsBtn->setIcon(icon::coins());
    coinsBtn->setIconSize(QSize(20, 20));
    coinsBtn->setCheckable(true);
    coinsBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *sendBtn = new Button("  Send", ButtonRole::Menu);
    sendBtn->setIcon(icon::send());
    sendBtn->setIconSize(QSize(20, 20));
    sendBtn->setCheckable(true);
    sendBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *recvBtn = new Button("  Receive", ButtonRole::Menu);
    recvBtn->setIcon(icon::receive());
    recvBtn->setIconSize(QSize(20, 20));
    recvBtn->setCheckable(true);
    recvBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *settingsBtn = new Button("  Settings", ButtonRole::Menu);
    settingsBtn->setIcon(icon::settings());
    settingsBtn->setIconSize(QSize(20, 20));
    settingsBtn->setCheckable(true);

    auto *btnGroup = new QButtonGroup(this);
    btnGroup->setExclusive(true);
    btnGroup->addButton(historyBtn);
    btnGroup->addButton(coinsBtn);
    btnGroup->addButton(sendBtn);
    btnGroup->addButton(recvBtn);
    btnGroup->addButton(settingsBtn);
    settingsBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_menu->push(historyBtn)
        ->pushSpacer(resolve(Spacing::S))
        ->push(coinsBtn)
        ->pushSpacer(resolve(Spacing::S))
        ->push(sendBtn)
        ->pushSpacer(resolve(Spacing::S))
        ->push(recvBtn)
        ->pushSpacer()
        ->push(settingsBtn);

    connect(historyBtn, &QPushButton::clicked, m_controller, &AccountController::historyClicked,
            qontrol::UNIQUE);
    connect(coinsBtn, &QPushButton::clicked, m_controller, &AccountController::coinsClicked,
            qontrol::UNIQUE);
    connect(sendBtn, &QPushButton::clicked, m_controller, &AccountController::sendClicked,
            qontrol::UNIQUE);
    connect(recvBtn, &QPushButton::clicked, m_controller, &AccountController::receiveClicked,
            qontrol::UNIQUE);
    connect(settingsBtn, &QPushButton::clicked, m_controller, &AccountController::settingsClicked,
            qontrol::UNIQUE);

    // Create screen container
    auto *container = new qontrol::Column;
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    container->layout()->setContentsMargins(0, 0, 0, 0);
    m_screen_container = container;

    // Wrap menu + screen container in horizontal layout
    auto *contentWidget = (new qontrol::Row)->push(m_menu)->push(m_screen_container);
    contentWidget->layout()->setContentsMargins(0, 0, 0, 0);
    contentWidget->layout()->setSpacing(0);

    // Create status bar
    m_status_bar = new StatusBar(m_controller, this);

    // Connect status bar signals
    connect(m_controller, &AccountController::scanProgress, m_status_bar,
            &StatusBar::onUpdateScanProgress, qontrol::UNIQUE);
    connect(m_controller, &AccountController::waitingForBlocks, m_status_bar,
            &StatusBar::onUpdateWaitingForBlocks, qontrol::UNIQUE);

    // Create main vertical layout with content + status bar
    auto *mainCol = (new qontrol::Column)->push(contentWidget)->push(m_status_bar);
    mainCol->layout()->setContentsMargins(0, 0, 0, 0);
    mainCol->layout()->setSpacing(0);
    setLayout(mainCol->layout());
}

void AccountWidget::loadPanel(qontrol::Panel *panel) {
    if (panel == nullptr)
        return;

    // Remove current panel if exists
    if (m_current_panel != nullptr) {
        auto *currentWidget = m_current_panel->widget();
        if (currentWidget != nullptr) {
            m_screen_container->layout()->removeWidget(currentWidget);
            currentWidget->setVisible(false);
        }
    }

    // Load new panel
    m_current_panel = panel;
    auto *widget = panel->widget();
    if (widget != nullptr) {
        m_screen_container->layout()->addWidget(widget);
        widget->setVisible(true);
    }
}

auto AccountWidget::controller() -> AccountController * {
    return m_controller;
}

AccountWidget::~AccountWidget() = default;
