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

// The panels go before the controller their signing step uses.
MusigAccountWidget::~MusigAccountWidget() {
    delete m_shell;
}

auto MusigAccountWidget::controller() -> MusigAccountController * {
    return m_controller;
}

auto MusigAccountWidget::shell() const -> account::AccountShell * {
    return m_shell;
}

void MusigAccountWidget::initUI() {
    auto entries = account::accountMenuEntries();
    entries.insert(entries.size() - 1,
                   {.name = "psbt", .label = TR("musig-menu-psbt"), .icon = icon::history()});
    m_shell =
        new account::AccountShell(std::move(entries), new MusigStatusBar(m_controller, this), this);
    m_controller->setShell(m_shell);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_shell->widget());
}
