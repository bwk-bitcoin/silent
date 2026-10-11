#include "HotRouteFlow.h"

#include "catalog/Button.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "theme/Palette.h"

#include <QLineEdit>
#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::flow {

HotRouteFlow::HotRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row,
                           QString psbt)
    : SigningRouteFlow(module, std::move(request), std::move(row), std::move(psbt)) {
    init();
    doConnect();
    view();
}

HotRouteFlow::~HotRouteFlow() {
    m_password_input->clear();
}

void HotRouteFlow::start() {
    QString prompt = copy().hotPrompt.arg(row().alias);
    m_password_prompt->setText(prompt);
    m_password_prompt->setVisible(!prompt.isEmpty());
    m_password_error->setVisible(false);
    showPassword();
}

void HotRouteFlow::init() {
    initPassword();
    initBusy();
}

void HotRouteFlow::doConnect() {
    connectPassword();
}

void HotRouteFlow::view() {
    m_password_step = viewPassword();
    hideBusy();
}

// ===== Password view =====

void HotRouteFlow::initPassword() {
    m_password_prompt = new catalog::Label(catalog::LabelRole::Body);
    m_password_prompt->setTone(catalog::LabelTone::Secondary);
    m_password_prompt->setWordWrap(true);
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_password_proceed = hotProceedButton(request().context);
    m_password_actions = new catalog::ModalActions;
    m_password_actions->addAction(m_password_proceed);
}

void HotRouteFlow::connectPassword() {
    connect(m_password_input, &QLineEdit::textEdited, this, &HotRouteFlow::onPasswordEdited,
            qontrol::UNIQUE);
    connect(m_password_input, &QLineEdit::returnPressed, this, &HotRouteFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_password_proceed, &QPushButton::clicked, this, &HotRouteFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(module(), &KeysModule::hotSigned, this, &HotRouteFlow::onHotSigned, qontrol::UNIQUE);
    connect(module(), &KeysModule::hotSpDescriptorFetched, this,
            &HotRouteFlow::onHotDescriptorFetched, qontrol::UNIQUE);
    connect(module(), &KeysModule::hotXpubFetched, this, &HotRouteFlow::onHotXpubFetched,
            qontrol::UNIQUE);
}

auto HotRouteFlow::viewPassword() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_password_prompt)
        ->push(new catalog::Field(TR("flow-password"), m_password_input))
        ->push(m_password_error);
}

// An empty field with the focus in it, and the error kept if one was just shown.
void HotRouteFlow::showPassword() {
    m_view = View::Password;
    m_password_input->clear();
    onPasswordEdited();
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_password_step);
    viewSlots()->setFooter(m_password_actions);
    emit viewChanged(copy().title, ModalWidth::S);
    m_password_input->setFocus();
}

void HotRouteFlow::proceedPassword() {
    if (m_password_input->text().isEmpty()) {
        return;
    }
    QString password = m_password_input->text();
    m_password_input->clear();
    m_password_error->setVisible(false);
    m_hot_req = module()->nextReqId();
    m_busy_step->setTitle(copy().hotBusy);
    m_busy_step->setSubtitle(TR("flow-hot-busy-with").arg(row().alias));
    showBusy();
    switch (request().context) {
    case Context::Sign:
        module()->signHot(m_hot_req, row().fingerprint, password, psbt());
        break;
    case Context::Fetch:
        module()->fetchHotSpDescriptor(m_hot_req, row().fingerprint, password);
        break;
    case Context::FetchXpub:
        module()->fetchHotXpub(m_hot_req, row().fingerprint, password, request().path);
        break;
    case Context::Verify:
        // A hot key has no screen: verify rows never pick it.
        break;
    }
    password.fill(QChar());
    password.clear();
}

// ===== Busy view =====

void HotRouteFlow::initBusy() {
    m_busy_step = new catalog::ModalStatus;
    m_busy_step->setSpinner();
    m_busy_step->setPadding(resolve(Spacing::M), resolve(Spacing::M));
}

void HotRouteFlow::showBusy() {
    m_view = View::Busy;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_busy_step);
    viewSlots()->setFooter(nullptr);
    emit viewChanged(copy().title, ModalWidth::S);
}

void HotRouteFlow::hideBusy() {
    viewSlots()->keep(m_busy_step);
}

// ===== Slots =====

void HotRouteFlow::onNextClicked() {
    switch (m_view) {
    case View::Password:
        proceedPassword();
        return;
    case View::Busy:
        return;
    }
}

void HotRouteFlow::onPasswordEdited() {
    bool empty = m_password_input->text().isEmpty();
    if (!empty) {
        m_password_error->setVisible(false);
    }
    m_password_proceed->setEnabled(!empty);
    m_password_actions->setReason(empty ? TR("flow-password-reason") : QString());
}

void HotRouteFlow::onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex,
                               const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    if (is_ok) {
        emit psbtSigned(psbt_v2_hex);
        return;
    }
    if (wrong_password) {
        m_password_error->setText(TR("flow-password-wrong"));
        m_password_error->setVisible(true);
        showPassword();
        return;
    }
    emit failed(error);
}

void HotRouteFlow::onHotDescriptorFetched(ReqId req_id, bool is_ok, const QString &descriptor,
                                          const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    if (is_ok) {
        emit descriptorFetched(descriptor);
        return;
    }
    if (wrong_password) {
        m_password_error->setText(TR("flow-password-wrong"));
        m_password_error->setVisible(true);
        showPassword();
        return;
    }
    emit failed(error);
}

void HotRouteFlow::onHotXpubFetched(ReqId req_id, bool is_ok, const QString &xpub,
                                    const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    if (is_ok) {
        emit xpubFetched(xpub);
        return;
    }
    if (wrong_password) {
        m_password_error->setText(TR("flow-password-wrong"));
        m_password_error->setVisible(true);
        showPassword();
        return;
    }
    emit failed(error);
}

} // namespace keys::flow
