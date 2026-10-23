#include "RemoveFlow.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>
#include <utility>

namespace modal {

RemoveFlow::RemoveFlow(QString name) : m_name(std::move(name)) {
    init();
    doConnect();
    view();
}

void RemoveFlow::init() {
    m_question =
        new catalog::Label(TR("settings-remove-question").arg(m_name), catalog::LabelRole::Body);
    m_question->setWordWrap(true);
    m_warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_warning->setText(TR("settings-remove-warning"));
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::Back);
    m_remove_btn = catalog::btnRemovePlugin();
    m_actions->addAction(m_remove_btn);
}

void RemoveFlow::doConnect() {
    connect(m_remove_btn, &QPushButton::clicked, this, &RemoveFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_actions, &catalog::ModalActions::backClicked, this, &RemoveFlow::onBackClicked,
            qontrol::UNIQUE);
}

void RemoveFlow::view() {
    m_body = (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_question)->push(m_warning);
}

void RemoveFlow::start() {
    viewSlots()->setHeader(nullptr);
    viewSlots()->setWideBody(m_body);
    viewSlots()->setFooter(m_actions);
    emit viewChanged(TR("settings-remove-plugin"), ModalWidth::Wizard);
}

void RemoveFlow::onBackClicked() {
    emit back();
}

void RemoveFlow::onNextClicked() {
    emit confirmed();
}

} // namespace modal
