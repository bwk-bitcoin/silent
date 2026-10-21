#include "KeysInstance.h"
#include "KeysModule.h"
#include "KeysTab.h"
#include "i18n/Tr.h"
#include <QEvent>

namespace keys {

KeysInstance::KeysInstance(KeysModule *module) : m_module(module) {
}

KeysInstance::~KeysInstance() {
    stop();
}

auto KeysInstance::id() const -> QString {
    return QStringLiteral("keys");
}

auto KeysInstance::openPage() -> bool {
    auto *page = new KeysTab(m_module);
    m_tab_id = Host::get()->openTab(page, this, Host::PagePlacement::Module);
    if (m_tab_id == 0) {
        delete page;
        return false;
    }
    m_page = page;
    page->installEventFilter(this);
    Host::get()->setTabTitle(m_tab_id, TR("main-keys-tab"));
    return true;
}

void KeysInstance::stop() {
    m_module->setTabShown(false);
    if (m_page != nullptr) {
        // The host removes the page now, but deferred deletion must not retain the module.
        m_page->removeEventFilter(this);
    }
    if (m_tab_id != 0) {
        Host::get()->closeTab(m_tab_id);
        m_tab_id = 0;
    }
    delete m_page.data();
    m_page.clear();
}

auto KeysInstance::account() -> IAccount * {
    return nullptr;
}

auto KeysInstance::feed() -> IFeed * {
    return nullptr;
}

auto KeysInstance::signer() -> ISigner * {
    return nullptr;
}

auto KeysInstance::theme() -> IThemeProvider * {
    return nullptr;
}

auto KeysInstance::eventFilter(QObject *watched, QEvent *event) -> bool {
    if (watched == m_page && event->type() == QEvent::LanguageChange) {
        Host::get()->setTabTitle(m_tab_id, TR("main-keys-tab"));
    }
    return QObject::eventFilter(watched, event);
}

} // namespace keys
