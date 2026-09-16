#include "UsbConfirm.h"

#include "catalog/Button.h"
#include "catalog/feedback/ModalStatus.h"
#include "i18n/Tr.h"
#include "views/utils.h"

#include <common.h>

using catalog::Button;
using catalog::ModalStatus;

namespace keys::modal {

UsbConfirm::UsbConfirm(const QString &device_name, QWidget *parent) : m_device_name(device_name) {
    Q_UNUSED(parent);
    setWindowTitle(m_device_name);
    init();
    doConnect();
    view();
}

void UsbConfirm::init() {
    m_status = new ModalStatus;
    m_status->setState(ModalStatus::State::Signing);
    m_status->setSubtitle(TR("confirm-send-device-step").arg(m_device_name));

    m_cancel_btn = new Button(TR("common-cancel"));
}

void UsbConfirm::doConnect() {
    connect(m_cancel_btn, &QPushButton::clicked, this, &UsbConfirm::onCancelClicked,
            qontrol::UNIQUE);
}

void UsbConfirm::view() {
    auto *btnRow = (new qontrol::Row)->pushSpacer()->push(m_cancel_btn)->pushSpacer();
    auto *col = (new qontrol::Column)
                    ->push(m_status)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(btnRow)
                    ->pushSpacer();
    setMainWidget(margin(col));
}

void UsbConfirm::onCancelClicked() {
    emit cancelled();
    reject();
}

void UsbConfirm::onSigned() {
    accept();
}

void UsbConfirm::onFailed(const QString &message) {
    m_status->setState(ModalStatus::State::Error);
    m_status->setSubtitle(message);
}

} // namespace keys::modal
