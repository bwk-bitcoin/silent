#include "RegisterDevice.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QLabel>
#include <Qontrol>
#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;

RegisterDevice::RegisterDevice(const QString &name, const QString &fingerprint,
                               const QString &network, const QStringList &aliases)
    : m_name(name),
      m_fingerprint(fingerprint),
      m_network(network),
      m_aliases(aliases) {
    init();
    doConnect();
    view();
    onAliasEdited();
}

void RegisterDevice::init() {
    m_alias = new catalog::Input;
    m_alias->setPlaceholderText(TR("keys-register-alias-placeholder").arg(m_name));
    m_taken = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Error);
    m_taken->setVisible(false);
    m_register = catalog::btnRegister();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_register);
}

void RegisterDevice::doConnect() {
    connect(m_alias, &QLineEdit::textEdited, this, &RegisterDevice::onAliasEdited, qontrol::UNIQUE);
    connect(m_alias, &QLineEdit::returnPressed, this, &RegisterDevice::onRegisterClicked,
            qontrol::UNIQUE);
    connect(m_register, &QPushButton::clicked, this, &RegisterDevice::onRegisterClicked,
            qontrol::UNIQUE);
}

void RegisterDevice::view() {
    auto *usbIcon = new QLabel;
    usbIcon->setPixmap(icon::usb(IconColor::Accent).pixmap(metric::ICON_SIZE, metric::ICON_SIZE));
    auto *fingerprint = new Label(m_fingerprint, LabelRole::Mono);
    fingerprint->setTone(catalog::LabelTone::Secondary);
    fingerprint->setSize(catalog::LabelSize::Sm);
    auto *device = (new qontrol::Row)
                       ->spacing(resolve(Spacing::M))
                       ->push(usbIcon)
                       ->push((new qontrol::Column)
                                  ->spacing(resolve(Spacing::XXS))
                                  ->push(new Label(m_name, LabelRole::Section))
                                  ->push(fingerprint),
                              1)
                       ->push(catalog::pillNetwork(m_network));
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setPad(catalog::Card::Pad::M);
    card->setContent(device);

    auto *alias = (new qontrol::Column)
                      ->spacing(resolve(Spacing::XS))
                      ->push(new Label(TR("keys-register-alias"), LabelRole::InputLabel))
                      ->push(m_alias)
                      ->push(m_taken);

    auto *body = (new qontrol::Column)->spacing(resolve(Spacing::L))->push(card)->push(alias);
    showStep(addStep(TR("keys-register-title"), ModalWidth::XS, body, m_actions));
    m_alias->setFocus();
}

auto RegisterDevice::aliasTaken() const -> bool {
    QString alias = m_alias->text().trimmed();
    return !alias.isEmpty() && m_aliases.contains(alias);
}

void RegisterDevice::onAliasEdited() {
    bool taken = aliasTaken();
    bool changed = m_taken->isHidden() == taken;
    m_taken->setText(TR("keys-alias-taken").arg(m_alias->text().trimmed()));
    m_taken->setVisible(taken);
    m_register->setEnabled(!taken);
    m_actions->setReason(taken ? TR("keys-alias-taken-reason") : QString());
    if (changed) {
        // The note adds a line: the window takes its height.
        showStep(currentStep());
    }
}

void RegisterDevice::onRegisterClicked() {
    if (aliasTaken()) {
        return;
    }
    QString alias = m_alias->text().trimmed();
    emit registerConfirmed(alias.isEmpty() ? m_name : alias);
    accept();
}

} // namespace keys::modal
