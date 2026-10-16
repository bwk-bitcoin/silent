#include "HotRouteFlow.h"

#include "catalog/Button.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/feedback/ModalStatus.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/modals/Unseal.h"
#include "theme/Palette.h"

#include <QDialog>
#include <QTimer>
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

void HotRouteFlow::start() {
    showWaiting();
    // Once the flow is shown, so the prompt opens over it.
    QTimer::singleShot(0, this, &HotRouteFlow::onAskPassword);
}

void HotRouteFlow::init() {
    initWaiting();
}

void HotRouteFlow::doConnect() {
    connect(module(), &KeysModule::hotSigned, this, &HotRouteFlow::onHotSigned, qontrol::UNIQUE);
    connect(module(), &KeysModule::hotSpDescriptorFetched, this,
            &HotRouteFlow::onHotDescriptorFetched, qontrol::UNIQUE);
    connect(module(), &KeysModule::hotXpubFetched, this, &HotRouteFlow::onHotXpubFetched,
            qontrol::UNIQUE);
}

void HotRouteFlow::view() {
    hideWaiting();
}

// ===== Waiting view =====

void HotRouteFlow::initWaiting() {
    m_waiting_step = new catalog::ModalStatus;
    m_waiting_step->setSpinner();
    m_waiting_step->setPadding(resolve(Spacing::M), resolve(Spacing::M));
}

void HotRouteFlow::showWaiting() {
    m_waiting_step->setTitle(copy().hotBusy);
    m_waiting_step->setSubtitle(TR("flow-hot-busy-with").arg(row().alias));
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_waiting_step);
    viewSlots()->setFooter(nullptr);
    emit viewChanged(copy().title, ModalWidth::S);
}

void HotRouteFlow::hideWaiting() {
    viewSlots()->keep(m_waiting_step);
}

// The key's answer to the password: a wrong one asks again, any other one closes the
// prompt, to be delivered once it has.
void HotRouteFlow::answer(AnswerKind kind, bool is_ok, const QString &value, const QString &error,
                          bool wrong_password, bool musig_session) {
    if (m_unseal.isNull()) {
        return;
    }
    if (wrong_password) {
        m_unseal->retry(TR("flow-password-wrong"));
        return;
    }
    m_answer = Answer{.kind = kind,
                      .is_ok = is_ok,
                      .value = value,
                      .error = error,
                      .musig_session = musig_session};
    m_unseal->finish(true);
}

void HotRouteFlow::deliver(const Answer &answer) {
    if (!answer.is_ok) {
        emit failed(answer.error, answer.musig_session ? plugin::SignError::MusigSession
                                                       : plugin::SignError::Other);
        return;
    }
    switch (answer.kind) {
    case AnswerKind::Signed:
        emit psbtSigned(answer.value);
        return;
    case AnswerKind::Descriptor:
        emit descriptorFetched(answer.value, {});
        return;
    case AnswerKind::Xpub:
        emit xpubFetched(answer.value);
        return;
    }
}

// ===== Slots =====

void HotRouteFlow::onAskPassword() {
    QString prompt = copy().hotPrompt.arg(row().alias);
    auto *unseal = new modal::Unseal(copy().title, prompt, hotProceedButton(request().context));
    unseal->setBusy(copy().hotBusy, TR("flow-hot-busy-with").arg(row().alias));
    connect(unseal, &modal::Unseal::passwordEntered, this, &HotRouteFlow::onPasswordEntered,
            qontrol::UNIQUE);
    m_unseal = unseal;
    m_answer.reset();
    unseal->exec();
    delete unseal;
    m_hot_req = 0;
    if (m_answer.has_value()) {
        Answer held = std::move(m_answer.value());
        m_answer.reset();
        deliver(held);
        return;
    }
    // Closed without an answer.
    if (hasPicker()) {
        emit back();
    } else {
        emit cancelled();
    }
}

void HotRouteFlow::onPasswordEntered(const QString &password) {
    m_hot_req = module()->nextReqId();
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
}

void HotRouteFlow::onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex,
                               const QString &error, bool wrong_password, bool musig_session) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    answer(AnswerKind::Signed, is_ok, psbt_v2_hex, error, wrong_password, musig_session);
}

void HotRouteFlow::onHotDescriptorFetched(ReqId req_id, bool is_ok, const QString &descriptor,
                                          const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    answer(AnswerKind::Descriptor, is_ok, descriptor, error, wrong_password, false);
}

void HotRouteFlow::onHotXpubFetched(ReqId req_id, bool is_ok, const QString &xpub,
                                    const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    answer(AnswerKind::Xpub, is_ok, xpub, error, wrong_password, false);
}

} // namespace keys::flow
