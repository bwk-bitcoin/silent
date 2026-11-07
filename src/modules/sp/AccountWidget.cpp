#include "AccountWidget.h"
#include "AccountController.h"
#ifdef SILENT_MOCK_UI
#include "MockAccountController.h"
#endif
#include "StatusBar.h"
#include "account/AccountShell.h"

#include <QVBoxLayout>
#include <common.h>

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
    auto *status = new StatusBar(m_controller, this);
    connect(m_controller, &AccountController::scanProgress, status,
            &StatusBar::onUpdateScanProgress, qontrol::UNIQUE);
    connect(m_controller, &AccountController::waitingForBlocks, status,
            &StatusBar::onUpdateWaitingForBlocks, qontrol::UNIQUE);
    m_shell = new account::AccountShell(account::accountMenuEntries(), status, this);
    m_controller->setShell(m_shell);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_shell->widget());
}

auto AccountWidget::shell() const -> account::AccountShell * {
    return m_shell;
}

auto AccountWidget::controller() -> AccountController * {
    return m_controller;
}

// The panels go before the controller their signing step uses.
AccountWidget::~AccountWidget() {
    delete m_shell;
}
