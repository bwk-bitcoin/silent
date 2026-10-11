#include "VerifyResultView.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace keys::flow {

VerifyResultView::VerifyResultView(const QString &address) {
    init(address);
    doConnect();
    view();
}

void VerifyResultView::setResult(SigningFlow::VerifyResult result, const QString &device) {
    switch (result) {
    case SigningFlow::VerifyResult::Matched:
        m_status->setIcon(icon::circleCheck, IconColor::Success);
        m_status->setTitle(TR("flow-verify-matched-title"));
        m_status->setSubtitle(TR("flow-verify-matched-body").arg(device));
        break;
    case SigningFlow::VerifyResult::Mismatch:
        m_status->setIcon(icon::alert, IconColor::Error);
        m_status->setTitle(TR("flow-verify-mismatch-title"));
        m_status->setSubtitle(TR("flow-verify-mismatch-body").arg(device));
        break;
    case SigningFlow::VerifyResult::Error:
        m_status->setIcon(icon::circleX, IconColor::Muted);
        m_status->setTitle(TR("flow-verify-error-title"));
        m_status->setSubtitle(TR("flow-verify-error-body").arg(device));
        break;
    }
}

auto VerifyResultView::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void VerifyResultView::init(const QString &address) {
    m_status = new catalog::ModalStatus;
    m_status->setPadding(resolve(Spacing::S), 0);
    m_address = new catalog::ChunkedAddress;
    m_address->setAddress(address);
    m_retry = catalog::btnTryAgain();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_retry);
}

void VerifyResultView::doConnect() {
    connect(m_retry, &QPushButton::clicked, this, &VerifyResultView::retryClicked, qontrol::UNIQUE);
}

void VerifyResultView::view() {
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setPad(catalog::Card::Pad::M);
    card->setContent(m_address);
    (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_status)->push(card)->into(this);
}

} // namespace keys::flow
