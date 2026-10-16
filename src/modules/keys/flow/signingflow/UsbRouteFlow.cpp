#include "UsbRouteFlow.h"

#include "LiveDevice.h"
#include "VerifyResultView.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/KeysModule.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QDebug>
#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::flow {

namespace {

// bwk-hwi folds its error kinds into text at the device service boundary, so the kind
// is read back from its own messages (bwk hwi Error's Display).
auto usbErrorFor(const QString &message) -> UsbRouteFlow::UsbError {
    if (message.contains(QStringLiteral("User refused"), Qt::CaseInsensitive)) {
        return UsbRouteFlow::UsbError::Rejected;
    }
    if (message.contains(QStringLiteral("Device disconnected"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("Device not found"), Qt::CaseInsensitive)) {
        return UsbRouteFlow::UsbError::Disconnected;
    }
    return UsbRouteFlow::UsbError::Failed;
}

} // namespace

UsbRouteFlow::UsbRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row,
                           QString psbt)
    : SigningRouteFlow(module, std::move(request), std::move(row), std::move(psbt)) {
    init();
    doConnect();
    view();
}

UsbRouteFlow::~UsbRouteFlow() {
    releaseDevice();
}

void UsbRouteFlow::start() {
    // A device that came back after an unplug has a new instance id.
    auto device = liveDeviceById(row().device_id);
    if (!device.has_value()) {
        device = liveDevice(row().fingerprint);
    }
    if (!device.has_value()) {
        showUsbError(UsbError::Disconnected);
        return;
    }
    m_device = device->signer;
    m_device_instance = device->instanceId;

    QString address;
    switch (request().context) {
    case Context::Sign: {
        auto payload = ::usb_sign_payload(rust::String(psbt().toStdString()));
        if (!payload.is_ok) {
            emit failed(KeysModule::toQt(payload.error));
            return;
        }
        connect(m_device, &ISigner::signed_, this, &UsbRouteFlow::onDeviceSigned, qontrol::UNIQUE);
        connect(m_device, &ISigner::error, this, &UsbRouteFlow::onDeviceError, qontrol::UNIQUE);
        m_device_req = m_device->signWithDescriptor(KeysModule::toQt(payload.psbt_base64),
                                                    request().descriptor);
        break;
    }
    case Context::Verify:
        connect(m_device, &ISigner::addressDisplayed, this, &UsbRouteFlow::onDeviceAddressDisplayed,
                qontrol::UNIQUE);
        connect(m_device, &ISigner::error, this, &UsbRouteFlow::onDeviceError, qontrol::UNIQUE);
        m_device_req =
            m_device->displayAddress(request().descriptor, request().path, request().address);
        address = request().address;
        break;
    case Context::Fetch:
        // A descriptor only comes back over QR codes or from a hot key.
        emit failed(TR("keys-sp-descriptor-unavailable"));
        return;
    case Context::FetchXpub:
        connect(m_device, &ISigner::xpub, this, &UsbRouteFlow::onDeviceXpub, qontrol::UNIQUE);
        connect(m_device, &ISigner::error, this, &UsbRouteFlow::onDeviceError, qontrol::UNIQUE);
        m_device_req = m_device->getXpub(request().path);
        break;
    }
    m_usb_status->setSpinner();
    m_usb_status->setTitle(copy().usbTitle);
    m_usb_status->setSubtitle(copy().usbWait.arg(deviceLabel()));
    m_usb_address->setAddress(address);
    m_usb_address_card->setVisible(!address.isEmpty());
    showUsb();
}

void UsbRouteFlow::init() {
    initUsb();
    initVerifyResult();
}

void UsbRouteFlow::doConnect() {
    connectUsb();
    connectVerifyResult();
}

void UsbRouteFlow::view() {
    m_usb_step = viewUsb();
    // Kept: a route that fails at once shows no view.
    hideUsb();
    hideVerifyResult();
}

// ===== USB view =====

void UsbRouteFlow::initUsb() {
    m_usb_status = new catalog::ModalStatus;
    m_usb_status->setPadding(resolve(Spacing::M), 0);
    m_usb_address = new catalog::ChunkedAddress;
    m_usb_address_card = new catalog::Card(catalog::Card::Role::Inset);
    m_usb_address_card->setPad(catalog::Card::Pad::M);
    m_usb_address_card->setContent(m_usb_address);
    m_usb_address_card->setVisible(false);
    m_usb_retry = catalog::btnTryAgain();
    m_usb_actions = new catalog::ModalActions;
    m_usb_actions->setBack(hasPicker() ? catalog::ModalActions::Back::OtherSigner
                                       : catalog::ModalActions::Back::None);
    m_usb_actions->addAction(m_usb_retry);
}

void UsbRouteFlow::connectUsb() {
    connect(m_usb_actions, &catalog::ModalActions::backClicked, this, &UsbRouteFlow::onBackClicked,
            qontrol::UNIQUE);
    connect(m_usb_retry, &QPushButton::clicked, this, &UsbRouteFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &UsbRouteFlow::onInstanceRemoved, qontrol::UNIQUE);
}

auto UsbRouteFlow::viewUsb() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_usb_status)
        ->push(m_usb_address_card);
}

void UsbRouteFlow::showUsb() {
    m_view = View::Usb;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_usb_step);
    viewSlots()->setFooter(nullptr);
    emit viewChanged(copy().usbTitle, ModalWidth::S);
}

// The failure with Back to the other signers and Try Again under it.
void UsbRouteFlow::showUsbError(UsbError error) {
    m_view = View::Usb;
    QString device = deviceLabel();
    m_usb_status->setIcon(icon::circleX, IconColor::Error);
    switch (error) {
    case UsbError::Rejected:
        m_usb_status->setTitle(TR("flow-usb-rejected-title"));
        m_usb_status->setSubtitle(TR("flow-usb-rejected-body").arg(device));
        break;
    case UsbError::Disconnected:
        m_usb_status->setTitle(TR("flow-usb-disconnected-title"));
        m_usb_status->setSubtitle(TR("flow-usb-disconnected-body").arg(device));
        break;
    case UsbError::Failed:
        m_usb_status->setTitle(TR("flow-usb-failed-title"));
        m_usb_status->setSubtitle(TR("flow-usb-failed-body").arg(device));
        break;
    }
    m_usb_address_card->setVisible(false);
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_usb_step);
    viewSlots()->setFooter(m_usb_actions);
    emit viewChanged(copy().usbTitle, ModalWidth::S);
}

void UsbRouteFlow::hideUsb() {
    viewSlots()->keep(m_usb_step);
    viewSlots()->keep(m_usb_actions);
}

void UsbRouteFlow::releaseDevice() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
    m_device = nullptr;
    m_device_instance.clear();
    m_device_req = 0;
}

// ===== Verify result view =====

void UsbRouteFlow::initVerifyResult() {
    m_result = new VerifyResultView(request().address);
}

void UsbRouteFlow::connectVerifyResult() {
    connect(m_result, &VerifyResultView::retryClicked, this, &UsbRouteFlow::onNextClicked,
            qontrol::UNIQUE);
}

void UsbRouteFlow::showVerifyResult(SigningFlow::VerifyResult result) {
    m_view = View::VerifyResult;
    emit verifyResultChanged(result);
    m_result->setResult(result, deviceLabel());
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_result);
    // A match needs no Try Again.
    viewSlots()->setFooter(result == SigningFlow::VerifyResult::Matched ? nullptr
                                                                        : m_result->actions());
    emit viewChanged(copy().title, ModalWidth::S);
}

void UsbRouteFlow::hideVerifyResult() {
    viewSlots()->keep(m_result);
    viewSlots()->keep(m_result->actions());
}

// ===== Slots =====

void UsbRouteFlow::onBackClicked() {
    switch (m_view) {
    case View::Usb:
        emit back();
        return;
    case View::VerifyResult:
        return;
    }
}

void UsbRouteFlow::onNextClicked() {
    switch (m_view) {
    case View::Usb:
        start();
        return;
    case View::VerifyResult:
        emit verifyResultChanged(std::nullopt);
        start();
        return;
    }
}

void UsbRouteFlow::onDeviceSigned(ReqId req_id, const QString &psbt_base64) {
    if (m_device_req == 0 || req_id != m_device_req) {
        return;
    }
    releaseDevice();
    auto merged = ::usb_merge_signed(rust::String(psbt().toStdString()),
                                     rust::String(psbt_base64.toStdString()));
    if (!merged.is_ok) {
        emit failed(KeysModule::toQt(merged.error));
        return;
    }
    emit psbtSigned(KeysModule::toQt(merged.psbt_v2_hex));
}

void UsbRouteFlow::onDeviceXpub(ReqId req_id, const QString &xpub) {
    if (m_device_req == 0 || req_id != m_device_req) {
        return;
    }
    releaseDevice();
    emit xpubFetched(xpub);
}

void UsbRouteFlow::onDeviceAddressDisplayed(ReqId req_id, [[maybe_unused]] const QString &address) {
    if (m_device_req == 0 || req_id != m_device_req) {
        return;
    }
    releaseDevice();
    showVerifyResult(SigningFlow::VerifyResult::Matched);
}

void UsbRouteFlow::onDeviceError(std::optional<ReqId> req_id, const QString &message,
                                 plugin::SignError kind) {
    if (m_device_req == 0 || !req_id.has_value() || req_id.value() != m_device_req) {
        return;
    }
    releaseDevice();
    qWarning() << "keys: device:" << message;
    // A retry over the same MuSig2 session cannot succeed: the caller restarts.
    if (kind == plugin::SignError::MusigSession) {
        emit failed(message, kind);
        return;
    }
    auto error = usbErrorFor(message);
    // Refusing to confirm an address is the device's answer that it does not match.
    if (request().context == Context::Verify && error == UsbError::Rejected) {
        showVerifyResult(SigningFlow::VerifyResult::Mismatch);
        return;
    }
    showUsbError(error);
}

void UsbRouteFlow::onInstanceRemoved(const QString &id) {
    if (m_device_req == 0 || id != m_device_instance) {
        return;
    }
    releaseDevice();
    showUsbError(UsbError::Disconnected);
}

} // namespace keys::flow
