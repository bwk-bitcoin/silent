#include "RemoveFlow.h"

#include "KeyRecord.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "theme/Palette.h"

#include <QStringList>
#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;

RemoveFlow::RemoveFlow(KeysModule *module, QString fingerprint)
    : KeyDetailFlow(module, std::move(fingerprint)) {
    init();
    doConnect();
    view();
}

void RemoveFlow::start() {
    showRemove();
}

void RemoveFlow::init() {
    initRemove();
}

void RemoveFlow::doConnect() {
    connectRemove();
}

void RemoveFlow::view() {
    m_remove_body = viewRemove();
    // Kept: a key already gone shows no view.
    hideRemove();
}

// ===== Remove view =====

void RemoveFlow::initRemove() {
    m_remove_question = new Label(LabelRole::Body);
    m_remove_question->setWordWrap(true);
    m_remove_accounts = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_remove_hot = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_remove_hot->setText(TR("keys-remove-hot"));
    m_remove_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_remove_confirm = catalog::btnRemoveKey();
    m_remove_actions = new catalog::ModalActions;
    m_remove_actions->setBack(catalog::ModalActions::Back::Back);
    m_remove_actions->addAction(m_remove_confirm);
}

void RemoveFlow::connectRemove() {
    connect(m_remove_actions, &catalog::ModalActions::backClicked, this, &RemoveFlow::onBackClicked,
            qontrol::UNIQUE);
    connect(m_remove_confirm, &QPushButton::clicked, this, &RemoveFlow::onNextClicked,
            qontrol::UNIQUE);
}

auto RemoveFlow::viewRemove() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_remove_question)
        ->push(m_remove_accounts)
        ->push(m_remove_hot)
        ->push(m_remove_error);
}

void RemoveFlow::showRemove() {
    auto current = keyInfo(module(), fingerprint());
    if (!current.has_value()) {
        return;
    }
    m_remove_question->setText(
        TR("keys-remove-question").arg(KeysModule::toQt(current->alias), fingerprint()));
    QStringList names;
    for (const auto &account : keyAccounts(module(), fingerprint())) {
        names.append(account.name);
    }
    m_remove_accounts->setTitle(names.size() == 1
                                    ? TR("keys-remove-accounts-one")
                                    : TR("keys-remove-accounts-many").arg(names.size()));
    m_remove_accounts->setText(
        TR("keys-remove-accounts-body").arg(names.join(QStringLiteral(", "))));
    m_remove_accounts->setVisible(!names.isEmpty());
    m_remove_hot->setVisible(current->has_hot_secret);
    m_remove_error->setVisible(false);
    showHeader(TR("keys-remove-title"));
    viewSlots()->setBody(m_remove_body);
    viewSlots()->setFooter(m_remove_actions);
    emit viewChanged(TR("keys-remove-title"), ModalWidth::L);
}

void RemoveFlow::hideRemove() {
    viewSlots()->keep(m_remove_body);
    viewSlots()->keep(m_remove_actions);
}

void RemoveFlow::removeKey() {
    bool registered =
        !module()->rust()->key_descriptors(rust::String(fingerprint().toStdString())).empty();
    auto result = module()->removeKey(fingerprint(), registered);
    if (!result.is_ok) {
        m_remove_error->setText(TR("keys-remove-failed").arg(KeysModule::toQt(result.error)));
        m_remove_error->setVisible(true);
        window()->adjustSize();
        return;
    }
    emit removed();
}

// ===== Slots =====

void RemoveFlow::onBackClicked() {
    emit back();
}

void RemoveFlow::onNextClicked() {
    removeKey();
}

} // namespace keys::modal
