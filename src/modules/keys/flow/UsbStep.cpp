#include "UsbStep.h"

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

UsbStep::UsbStep(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
}

void UsbStep::init() {
    m_status = new catalog::ModalStatus;
    m_status->setPadding(resolve(Spacing::M), 0);

    m_address = new catalog::ChunkedAddress;
    m_address_card = new catalog::Card(catalog::Card::Role::Inset);
    m_address_card->setPad(catalog::Card::Pad::M);
    m_address_card->setContent(m_address);
    m_address_card->setVisible(false);

    m_retry = catalog::btnTryAgain();
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::OtherSigner);
    m_actions->addAction(m_retry);
    m_actions->setVisible(false);
}

void UsbStep::doConnect() {
    connect(m_actions, &catalog::ModalActions::backClicked, this, &UsbStep::backClicked,
            qontrol::UNIQUE);
    connect(m_retry, &QPushButton::clicked, this, &UsbStep::retryClicked, qontrol::UNIQUE);
}

void UsbStep::view() {
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_status)
        ->push(m_address_card)
        ->into(this);
}

void UsbStep::showWaiting(const QString &title, const QString &text, const QString &address) {
    m_status->setSpinner();
    m_status->setTitle(title);
    m_status->setSubtitle(text);
    m_address->setAddress(address);
    m_address_card->setVisible(!address.isEmpty());
    m_actions->setVisible(false);
}

void UsbStep::showError(Error error, const QString &device) {
    m_status->setIcon(icon::circleX, IconColor::Error);
    switch (error) {
    case Error::Rejected:
        m_status->setTitle(TR("flow-usb-rejected-title"));
        m_status->setSubtitle(TR("flow-usb-rejected-body").arg(device));
        break;
    case Error::Disconnected:
        m_status->setTitle(TR("flow-usb-disconnected-title"));
        m_status->setSubtitle(TR("flow-usb-disconnected-body").arg(device));
        break;
    case Error::Failed:
        m_status->setTitle(TR("flow-usb-failed-title"));
        m_status->setSubtitle(TR("flow-usb-failed-body").arg(device));
        break;
    }
    m_address_card->setVisible(false);
    m_actions->setVisible(true);
}

void UsbStep::setBackVisible(bool visible) {
    m_actions->setBack(visible ? catalog::ModalActions::Back::OtherSigner
                               : catalog::ModalActions::Back::None);
}

auto UsbStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

} // namespace keys::flow
