#include "modules/musig/views/Settings.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/CopyField.h"
#include "catalog/display/Label.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <Qontrol>
#include <common.h>
#include <silent.h>
#include <string>

namespace musig::view {

using catalog::FieldGroup;
using catalog::FieldNote;
using catalog::FoldSection;

Settings::Settings(MusigAccountController *ctrl) : m_controller(ctrl) {
    if (m_controller == nullptr) {
        return;
    }
    init();
    doConnect();
    view();
}

void Settings::init() {
    m_electrum_url_input = new catalog::Input;
    m_electrum_url_input->setWidth(Size::XL);
    m_electrum_url_input->setPlaceholderText(TR("settings-placeholder-electrum"));
    m_electrum_url_input->setText(m_controller->electrumUrl());

    m_electrum_note = new FieldNote(QString(), FieldNote::Tone::Error);
    m_electrum_note->setVisible(false);

    m_descriptor = new catalog::CopyField(
        QString::fromStdString(std::string(m_controller->getDescriptor().c_str())));
    m_descriptor->setWrap(true);
    m_descriptor->setFixedWidth(resolve(Size::XL));

    m_cosigners = new catalog::Label(
        m_controller->cosignerFingerprints().join(QStringLiteral(", ")), catalog::LabelRole::Mono);

    m_save_btn = catalog::btnSaveSettings();
    m_delete_btn = catalog::btnDelete();
}

void Settings::doConnect() {
    connect(m_save_btn, &QPushButton::clicked, this, &Settings::onSaveClicked, qontrol::UNIQUE);
    connect(m_delete_btn, &QPushButton::clicked, this, &Settings::onDeleteClicked, qontrol::UNIQUE);
}

void Settings::view() {
    auto *connectionContent = (new FieldGroup)
                                  ->addField(TR("settings-electrum-server"), m_electrum_url_input)
                                  ->addNote(m_electrum_note);
    auto *connectionSection = new FoldSection(TR("settings-connection"));
    connectionSection->setContent(connectionContent);

    auto *accountContent = (new FieldGroup)
                               ->addField(TR("musig-settings-descriptor"), m_descriptor, true)
                               ->addField(TR("musig-settings-cosigners"), m_cosigners);
    auto *accountSection = new FoldSection(TR("musig-settings-account"));
    accountSection->setContent(accountContent);

    auto *buttonRow = (new qontrol::Row)
                          ->pushSpacer()
                          ->push(m_save_btn)
                          ->pushSpacer(resolve(Spacing::S))
                          ->push(m_delete_btn)
                          ->pushSpacer();

    auto *col = (new qontrol::Column)
                    ->pushSpacer(resolve(Spacing::XXL))
                    ->push(connectionSection)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::Separator)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(accountSection)
                    ->pushSpacer(resolve(Spacing::L))
                    ->push(buttonRow)
                    ->pushSpacer();

    auto *row = (new qontrol::Row)->pushSpacer()->push(col)->pushSpacer();
    setScreenContent(this, m_main_widget, dashboard(TR("settings-title"), row));
}

// ===== Slots =====

void Settings::onSaveClicked() {
    QString url = m_electrum_url_input->text().trimmed();
    auto result = m_controller->setElectrumUrl(url);
    if (!result.saved) {
        m_electrum_note->setText(TR("musig-settings-save-failed"));
        m_electrum_note->setVisible(true);
    } else if (!url.isEmpty() && !result.started) {
        m_electrum_note->setText(TR("musig-settings-invalid-url"));
        m_electrum_note->setVisible(true);
    } else {
        m_electrum_note->setVisible(false);
    }
}

void Settings::onDeleteClicked() {
    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
        return;
    }
    AppController::get()->deleteAccount(
        QString::fromStdString(std::string(account.value()->name().c_str())));
}

} // namespace musig::view
