#include "VerifyResultStep.h"

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

VerifyResultStep::VerifyResultStep(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
}

void VerifyResultStep::init() {
    m_status = new catalog::ModalStatus;
    m_status->setPadding(resolve(Spacing::S), 0);
    m_address = new catalog::ChunkedAddress;
    m_retry = catalog::btnTryAgain();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_retry);
}

void VerifyResultStep::doConnect() {
    connect(m_retry, &QPushButton::clicked, this, &VerifyResultStep::retryClicked, qontrol::UNIQUE);
}

void VerifyResultStep::view() {
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setPad(catalog::Card::Pad::M);
    card->setContent(m_address);
    (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_status)->push(card)->into(this);
}

void VerifyResultStep::setAddress(const QString &address) {
    m_address->setAddress(address);
}

void VerifyResultStep::setResult(Result result, const QString &device) {
    switch (result) {
    case Result::Matched:
        m_status->setIcon(icon::circleCheck, IconColor::Success);
        m_status->setTitle(TR("flow-verify-matched-title"));
        m_status->setSubtitle(TR("flow-verify-matched-body").arg(device));
        break;
    case Result::Mismatch:
        m_status->setIcon(icon::alert, IconColor::Error);
        m_status->setTitle(TR("flow-verify-mismatch-title"));
        m_status->setSubtitle(TR("flow-verify-mismatch-body").arg(device));
        break;
    case Result::Error:
        m_status->setIcon(icon::circleX, IconColor::Muted);
        m_status->setTitle(TR("flow-verify-error-title"));
        m_status->setSubtitle(TR("flow-verify-error-body").arg(device));
        break;
    }
    m_actions->setVisible(result != Result::Matched);
}

auto VerifyResultStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

} // namespace keys::flow
