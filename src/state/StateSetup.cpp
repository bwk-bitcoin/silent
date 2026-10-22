#include "StateSetup.h"

#include "statesetup/DeviceFlow.h"
#include "statesetup/PasswordFlow.h"

#include "catalog/SelectRow.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QEvent>
#include <common.h>

namespace state {

StateSetup::StateSetup(Network network) : m_network(network) {
    init();
    doConnect();
    view();
    showChoice();
}

void StateSetup::done(int result) {
    if (m_busy) {
        return;
    }
    closeFlow();
    catalog::Modal::done(result);
}

void StateSetup::init() {
    initChoice();
}

void StateSetup::doConnect() {
    connectChoice();
}

void StateSetup::view() {
    m_choice = viewChoice();
    retranslateUi();
}

// ===== Choice view =====

void StateSetup::initChoice() {
    m_choice_text = new catalog::Label(catalog::LabelRole::Body);
    m_choice_text->setWordWrap(true);
    m_device_choice = new catalog::SelectRow;
    m_device_choice->setIcon(icon::usb);
    m_password_choice = new catalog::SelectRow;
    m_password_choice->setIcon(icon::lock);
}

void StateSetup::connectChoice() {
    connect(m_device_choice, &catalog::SelectRow::clicked, this, &StateSetup::onDeviceChosen,
            qontrol::UNIQUE);
    connect(m_password_choice, &catalog::SelectRow::clicked, this, &StateSetup::onPasswordChosen,
            qontrol::UNIQUE);
}

auto StateSetup::viewChoice() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_choice_text)
        ->push((new qontrol::Column)
                   ->spacing(resolve(Spacing::S))
                   ->push(m_device_choice)
                   ->push(m_password_choice));
}

void StateSetup::showChoice() {
    setWindowTitle(TR("state-setup-title"));
    setHeader(nullptr);
    setBody(m_choice);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

// ===== Shared =====

void StateSetup::openFlow(StateSetupFlow *flow) {
    m_flow = flow;
    connect(flow, &StateSetupFlow::viewChanged, this, &StateSetup::onFlowViewChanged,
            qontrol::UNIQUE);
    connect(flow, &StateSetupFlow::back, this, &StateSetup::onFlowBack, qontrol::UNIQUE);
    connect(flow, &StateSetupFlow::busyChanged, this, &StateSetup::onFlowBusyChanged,
            qontrol::UNIQUE);
    connect(flow, &StateSetupFlow::done, this, &StateSetup::accept, qontrol::UNIQUE);
    setPage(flow);
    flow->start();
}

void StateSetup::closeFlow() {
    if (m_flow == nullptr) {
        return;
    }
    m_flow->disconnect(this);
    // The flow may still be in the slot that emitted.
    m_flow->deleteLater();
    m_flow = nullptr;
}

void StateSetup::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void StateSetup::retranslateUi() {
    m_choice_text->setText(TR("state-setup-text"));
    m_device_choice->setTitle(TR("state-choice-device"));
    m_device_choice->setSub(TR("state-choice-device-sub"));
    m_password_choice->setTitle(TR("state-choice-password"));
    m_password_choice->setSub(TR("state-choice-password-sub"));
    if (m_flow == nullptr) {
        setWindowTitle(TR("state-setup-title"));
    }
}

// ===== Slots =====

void StateSetup::onDeviceChosen() {
    if (m_flow == nullptr) {
        openFlow(new DeviceFlow(m_network));
    }
}

void StateSetup::onPasswordChosen() {
    if (m_flow == nullptr) {
        openFlow(new PasswordFlow);
    }
}

void StateSetup::onFlowViewChanged(const QString &title, ModalWidth width) {
    setWindowTitle(title);
    setWidth(width);
}

void StateSetup::onFlowBack() {
    if (m_busy) {
        return;
    }
    showChoice();
    closeFlow();
}

void StateSetup::onFlowBusyChanged(bool busy) {
    m_busy = busy;
}

// ===== StateSetupFlow =====

StateSetupFlow::StateSetupFlow(QWidget *parent) : QWidget(parent) {
    m_slots = new catalog::ViewSlots;
    (new qontrol::Column)->push(m_slots)->into(this);
}

auto StateSetupFlow::viewSlots() const -> catalog::ViewSlots * {
    return m_slots;
}

} // namespace state
