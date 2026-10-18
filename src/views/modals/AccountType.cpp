#include "AccountType.h"

#include "AppController.h"
#include "catalog/SelectRow.h"
#include "i18n/Tr.h"
#include "interfaces/module.h"
#include "theme/Icon.h"

#include <Qontrol>
#include <common.h>

namespace modal {

namespace {

const char *const K_CREATOR = "creatorIndex";

// The icon a creator's module names in meta(), a puzzle piece for any other.
auto creatorIcon(const IModule *module) -> IconFn {
    auto name = module->meta().value(QStringLiteral("icon"));
    if (name == QStringLiteral("silent")) {
        return icon::silentMark;
    }
    if (name == QStringLiteral("users")) {
        return icon::users;
    }
    return icon::puzzle;
}

} // namespace

AccountType::AccountType(const QList<PluginRegistry::CreatorLauncher> &creators)
    : m_creators(creators) {
    init();
    doConnect();
    view();
}

void AccountType::init() {
    for (int i = 0; i < m_creators.size(); ++i) {
        auto *row = new catalog::SelectRow;
        row->setIcon(creatorIcon(m_creators.at(i).module));
        row->setTitle(m_creators.at(i).name);
        row->setTwoLine(true);
        row->setProperty(K_CREATOR, i);
        m_rows.append(row);
    }
}

void AccountType::doConnect() {
    for (auto *row : m_rows) {
        connect(row, &catalog::SelectRow::clicked, this, &AccountType::onCreatorClicked,
                qontrol::UNIQUE);
    }
}

void AccountType::onCreatorClicked() {
    const auto &creator = m_creators.at(sender()->property(K_CREATOR).toInt());
    // Closed first: a creator opens its own modal, which is not stacked over this one.
    accept();
    // Route through the registry so the created instance is attributed to its owning
    // plugin (required for clean disable later).
    auto *controller = AppController::get();
    auto *registry = controller != nullptr ? controller->pluginRegistry() : nullptr;
    if (registry != nullptr) {
        registry->startLauncherInstance(creator.module, creator.id);
    } else {
        creator.module->startInstance(creator.id);
    }
}

void AccountType::view() {
    auto *column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    for (auto *row : m_rows) {
        column->push(row);
    }
    setWideBody(column);
    setWidth(ModalWidth::S);
    retranslateUi();
}

void AccountType::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void AccountType::retranslateUi() {
    setWindowTitle(TR("account-type-title"));
}

} // namespace modal
