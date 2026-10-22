#include "modules/musig/MusigAccountWidget.h"

#include "account/AccountShell.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigStatusBar.h"
#include "theme/Icon.h"

#include <QVBoxLayout>
#include <common.h>
#include <utility>

MusigAccountWidget::MusigAccountWidget(const QString &account, QWidget *parent) : QWidget(parent) {
    m_controller = new MusigAccountController(account, this);
    initUI();
}

MusigAccountWidget::~MusigAccountWidget() = default;

auto MusigAccountWidget::controller() -> MusigAccountController * {
    return m_controller;
}

void MusigAccountWidget::loadPanel(qontrol::Panel *panel) {
    m_shell->loadPanel(panel);
}

void MusigAccountWidget::initUI() {
    auto entries = account::accountMenuEntries();
    entries.insert(entries.size() - 1,
                   {.name = "psbt", .label = TR("musig-menu-psbt"), .icon = icon::history()});
    m_shell =
        new account::AccountShell(std::move(entries), new MusigStatusBar(m_controller, this), this);
    connect(m_shell, &account::AccountShell::panelRequested, m_controller,
            &MusigAccountController::loadPanel, qontrol::UNIQUE);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_shell->widget());
}
