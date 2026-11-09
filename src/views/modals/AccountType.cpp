#include "AccountType.h"

#include "AppController.h"
#include "catalog/SelectRow.h"
#include "catalog/containers/ModalActions.h"
#include "i18n/Tr.h"
#include "interfaces/module.h"
#include "theme/Icon.h"

#include <Qontrol>
#include <common.h>

namespace modal {

namespace {

const char *const K_TYPE = "typeIndex";

// The icon a type's module names in meta(), a puzzle piece for any other.
auto typeIcon(const IModule *module) -> IconFn {
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

AccountType::AccountType(const QList<PluginRegistry::CreatorLauncher> &types) : m_types(types) {
    init();
    doConnect();
    view();
}

void AccountType::init() {
    initTypes();
    initChoice();
}

void AccountType::doConnect() {
    connectTypes();
    connectChoice();
}

void AccountType::view() {
    m_types_view = viewTypes();
    m_choice_view = viewChoice();
    hideChoice();
    retranslateUi();
    showTypes();
}

void AccountType::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void AccountType::retranslateUi() {
    m_create_row->setTitle(TR("account-type-create"));
    m_create_row->setSub(TR("account-type-create-sub"));
    m_restore_row->setTitle(TR("account-type-restore"));
    m_restore_row->setSub(TR("account-type-restore-sub"));
    setWindowTitle(title());
}

// ===== Types view =====

void AccountType::initTypes() {
    for (int i = 0; i < m_types.size(); ++i) {
        auto *row = new catalog::SelectRow;
        row->setIcon(typeIcon(m_types.at(i).module));
        row->setTitle(m_types.at(i).name);
        row->setTwoLine(true);
        row->setProperty(K_TYPE, i);
        m_type_rows.append(row);
    }
}

void AccountType::connectTypes() {
    for (auto *row : m_type_rows) {
        connect(row, &catalog::SelectRow::clicked, this, &AccountType::onTypeClicked,
                qontrol::UNIQUE);
    }
}

auto AccountType::viewTypes() -> QWidget * {
    auto *column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    for (auto *row : m_type_rows) {
        column->push(row);
    }
    return column;
}

void AccountType::showTypes() {
    m_view = View::Types;
    setWindowTitle(title());
    setHeader(nullptr);
    setWideBody(m_types_view);
    setFooter(nullptr);
    setWidth(ModalWidth::S);
}

// ===== Choice view =====

void AccountType::initChoice() {
    m_create_row = new catalog::SelectRow;
    m_create_row->setIcon(icon::plus);
    m_restore_row = new catalog::SelectRow;
    m_restore_row->setIcon(icon::folder);
    m_choice_actions = new catalog::ModalActions;
    m_choice_actions->setBack(catalog::ModalActions::Back::Back);
}

void AccountType::connectChoice() {
    connect(m_create_row, &catalog::SelectRow::clicked, this, &AccountType::onCreateClicked,
            qontrol::UNIQUE);
    connect(m_restore_row, &catalog::SelectRow::clicked, this, &AccountType::onRestoreClicked,
            qontrol::UNIQUE);
    connect(m_choice_actions, &catalog::ModalActions::backClicked, this,
            &AccountType::onBackClicked, qontrol::UNIQUE);
}

auto AccountType::viewChoice() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::S))
        ->push(m_create_row)
        ->push(m_restore_row);
}

void AccountType::showChoice() {
    m_view = View::Choice;
    setWindowTitle(title());
    setHeader(nullptr);
    setWideBody(m_choice_view);
    setFooter(m_choice_actions);
    setWidth(ModalWidth::S);
}

void AccountType::hideChoice() {
    keep(m_choice_view);
    keep(m_choice_actions);
}

// ===== Shared =====

auto AccountType::title() const -> QString {
    switch (m_view) {
    case View::Types:
        return TR("account-type-title");
    case View::Choice:
        return TR("account-type-choice-title").arg(m_types.at(m_type).name);
    }
    return {};
}

void AccountType::startEntry(const QString &id) {
    auto *module = m_types.at(m_type).module;
    // Closed first: an entry opens its own modal, which is not stacked over this one.
    accept();
    // Route through the registry so the created instance is attributed to its owning
    // plugin (required for clean disable later).
    auto *controller = AppController::get();
    auto *registry = controller != nullptr ? controller->pluginRegistry() : nullptr;
    if (registry != nullptr) {
        registry->startLauncherInstance(module, id);
    } else {
        module->startInstance(id);
    }
}

// ===== Slots =====

void AccountType::onBackClicked() {
    switch (m_view) {
    case View::Types:
        break;
    case View::Choice:
        showTypes();
        break;
    }
}

void AccountType::onTypeClicked() {
    m_type = sender()->property(K_TYPE).toInt();
    const auto &type = m_types.at(m_type);
    if (type.createId.isEmpty()) {
        startEntry(type.restoreId);
    } else if (type.restoreId.isEmpty()) {
        startEntry(type.createId);
    } else {
        showChoice();
    }
}

void AccountType::onCreateClicked() {
    startEntry(m_types.at(m_type).createId);
}

void AccountType::onRestoreClicked() {
    startEntry(m_types.at(m_type).restoreId);
}

} // namespace modal
