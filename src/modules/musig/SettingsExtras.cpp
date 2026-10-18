#include "modules/musig/SettingsExtras.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/CopyField.h"
#include "catalog/display/Label.h"
#include "catalog/form/FieldGroup.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/RegisterDescriptor.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>
#include <silent.h>
#include <string>

namespace musig {

SettingsExtras::SettingsExtras(MusigAccountController *ctrl) : m_controller(ctrl) {
    init();
    doConnect();
    view();
}

void SettingsExtras::init() {
    m_descriptor = new catalog::CopyField(
        QString::fromStdString(std::string(m_controller->getDescriptor().c_str())));
    m_descriptor->setWrap(true);
    m_descriptor->setFixedWidth(resolve(Size::XL));

    m_cosigners = new catalog::Label(
        m_controller->cosignerFingerprints().join(QStringLiteral(", ")), catalog::LabelRole::Mono);

    m_delete_btn = catalog::btnDelete();
    m_register_btn = catalog::btnRegisterDescriptor();
}

void SettingsExtras::doConnect() {
    connect(m_delete_btn, &QPushButton::clicked, this, &SettingsExtras::onDeleteClicked,
            qontrol::UNIQUE);
    connect(m_register_btn, &QPushButton::clicked, this, &SettingsExtras::onRegisterClicked,
            qontrol::UNIQUE);
}

void SettingsExtras::view() {
    auto *accountContent = (new catalog::FieldGroup)
                               ->addField(TR("musig-settings-descriptor"), m_descriptor, true)
                               ->addField(TR("musig-settings-cosigners"), m_cosigners)
                               ->addField(TR("musig-settings-devices"), m_register_btn);
    auto *accountSection = new catalog::FoldSection(TR("musig-settings-account"));
    accountSection->setContent(accountContent);

    auto *deleteRow = (new qontrol::Row)->pushSpacer()->push(m_delete_btn)->pushSpacer();

    auto *col = (new qontrol::Column)
                    ->push(new catalog::Separator)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(accountSection)
                    ->pushSpacer(resolve(Spacing::L))
                    ->push(deleteRow);
    setLayout(col->layout());
}

// ===== Slots =====

void SettingsExtras::onRegisterClicked() {
    AppController::execModal(new modal::RegisterDescriptor(m_controller));
}

void SettingsExtras::onDeleteClicked() {
    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
        return;
    }
    AppController::get()->deleteAccount(
        QString::fromStdString(std::string(account.value()->name().c_str())));
}

} // namespace musig
