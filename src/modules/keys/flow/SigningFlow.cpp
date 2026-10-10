#include "SigningFlow.h"

#include "CameraStep.h"
#include "PasswordStep.h"
#include "QrDisplayStep.h"
#include "SdStep.h"
#include "UsbStep.h"

#include "catalog/containers/ModalActions.h"
#include "catalog/feedback/ModalStatus.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/QrRoute.h"
#include "theme/Palette.h"

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>
#include <utility>

#include <common.h>

namespace keys::flow {

namespace {

const QString UNSIGNED_SUFFIX = QStringLiteral("-unsigned.psbt");
const QString SIGNED_SUFFIX = QStringLiteral("-signed.psbt");
const QString PSBT_EXTENSION = QStringLiteral(".psbt");

// bwk-hwi folds its error kinds into text at the device service boundary, so the kind
// is read back from its own messages (bwk hwi Error's Display).
auto usbErrorFor(const QString &message) -> UsbStep::Error {
    if (message.contains(QStringLiteral("User refused"), Qt::CaseInsensitive)) {
        return UsbStep::Error::Rejected;
    }
    if (message.contains(QStringLiteral("Device disconnected"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("Device not found"), Qt::CaseInsensitive)) {
        return UsbStep::Error::Disconnected;
    }
    return UsbStep::Error::Failed;
}

auto unusableStatus(KeyUnusableReason reason) -> SignerStatus {
    switch (reason) {
    case KeyUnusableReason::SpOutputsPending:
        return SignerStatus::Waiting;
    case KeyUnusableReason::NotRegistered:
        return SignerStatus::Unregistered;
    case KeyUnusableReason::DeviceNotConnected:
        return SignerStatus::Disconnected;
    case KeyUnusableReason::NoHotSecret:
        return SignerStatus::NoHot;
    case KeyUnusableReason::NoRoute:
    case KeyUnusableReason::None:
    default:
        return SignerStatus::NoRoute;
    }
}

auto sameFingerprint(const QString &left, const QString &right) -> bool {
    return QString::compare(left, right, Qt::CaseInsensitive) == 0;
}

// The live device signer for `fingerprint`, and its host instance id.
auto liveDevice(const QString &fingerprint) -> std::optional<plugin::SignerEntry> {
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && sameFingerprint(entry.signer->fingerprint(), fingerprint)) {
            return entry;
        }
    }
    return std::nullopt;
}

auto liveDeviceById(const QString &instance_id) -> std::optional<plugin::SignerEntry> {
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.instanceId == instance_id) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace

SigningFlow::SigningFlow(KeysModule *module, Request request)
    : m_module(module),
      m_request(std::move(request)),
      m_copy(flowCopy(m_request.context)),
      m_psbt(m_request.psbt_v2_hex) {
    init();
    doConnect();
    view();
}

void SigningFlow::init() {
    m_picker = new PickerStep;
    m_usb = new UsbStep;
    m_usb->setBackVisible(hasPicker());
    m_sd = new SdStep;
    m_sd->setFileBase(m_request.wallet_name);
    m_password = new PasswordStep(hotProceedButton(m_request.context));
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::M), resolve(Spacing::M));
    m_qr = new QrDisplayStep;
    m_qr->setHint(m_copy.qrHint);
    m_camera = new CameraStep(m_module);
    m_camera->setHint(m_copy.scanHint);
    m_result = new VerifyResultStep;
    m_result->setAddress(m_request.address);
}

void SigningFlow::doConnect() {
    connect(m_picker, &PickerStep::rowPicked, this, &SigningFlow::onRowPicked, qontrol::UNIQUE);
    connect(m_usb, &UsbStep::backClicked, this, &SigningFlow::onUsbBack, qontrol::UNIQUE);
    connect(m_usb, &UsbStep::retryClicked, this, &SigningFlow::onUsbRetry, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &SigningFlow::onInstanceRemoved, qontrol::UNIQUE);
    connect(m_password, &PasswordStep::passwordEntered, this, &SigningFlow::onPasswordEntered,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotSigned, this, &SigningFlow::onHotSigned, qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotSpDescriptorFetched, this,
            &SigningFlow::onHotDescriptorFetched, qontrol::UNIQUE);
    connect(m_sd, &SdStep::saveClicked, this, &SigningFlow::onSdSaveClicked, qontrol::UNIQUE);
    connect(m_sd, &SdStep::loadClicked, this, &SigningFlow::onSdLoadClicked, qontrol::UNIQUE);
    connect(m_module, &KeysModule::sdSaved, this, &SigningFlow::onSdSaved, qontrol::UNIQUE);
    connect(m_module, &KeysModule::sdLoaded, this, &SigningFlow::onSdLoaded, qontrol::UNIQUE);
    connect(m_qr, &QrDisplayStep::nextClicked, this, &SigningFlow::onQrNext, qontrol::UNIQUE);
    connect(m_camera, &CameraStep::backClicked, this, &SigningFlow::onCameraBack, qontrol::UNIQUE);
    auto *qrRoute = m_module->qrRoute();
    connect(qrRoute, &QrRoute::psbtSigned, this, &SigningFlow::onQrSigned, qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::addressVerified, this, &SigningFlow::onQrVerified, qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::descriptorDecoded, this, &SigningFlow::onQrDescriptor,
            qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::scanFailed, this, &SigningFlow::onQrScanFailed, qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::failed, this, &SigningFlow::onQrFailed, qontrol::UNIQUE);
    connect(m_result, &VerifyResultStep::retryClicked, this, &SigningFlow::onVerifyRetry,
            qontrol::UNIQUE);
}

void SigningFlow::view() {
    m_picker_page = addStep(m_copy.title, ModalWidth::M, m_picker, nullptr);
    m_usb_page = addStep(m_copy.usbTitle, ModalWidth::S, m_usb, m_usb->actions());
    m_sd_page = addStep(m_copy.sdTitle, ModalWidth::M, m_sd, m_sd->actions());
    m_password_page = addStep(m_copy.title, ModalWidth::S, m_password, m_password->actions());
    m_busy_page = addStep(m_copy.title, ModalWidth::S, m_busy, nullptr);
    m_qr_page = addStep(m_copy.qrTitle, ModalWidth::XXL, m_qr, m_qr->actions());
    m_camera_page = addStep(m_copy.scanTitle, ModalWidth::XXL, m_camera, m_camera->actions());
    m_result_page = addStep(m_copy.title, ModalWidth::S, m_result, m_result->actions());
}

void SigningFlow::begin() {
    switch (m_request.context) {
    case Context::Sign: {
        auto rows = signRows();
        if (!rows.has_value()) {
            fail(TR("flow-plan-failed"));
            return;
        }
        m_rows = rows.value();
        showPicker();
        return;
    }
    case Context::Verify:
        m_rows = verifyRows();
        showPicker();
        return;
    case Context::Fetch:
        m_current = fetchRow();
        startRoute();
        return;
    }
}

auto SigningFlow::answered() const -> bool {
    return m_answered;
}

void SigningFlow::done(int result) {
    stopRoute();
    if (!m_answered) {
        m_answered = true;
        bool checked = m_verify_result.has_value() &&
                       m_verify_result.value() != VerifyResultStep::Result::Error;
        if (checked) {
            emit addressVerified(m_request.req_id,
                                 m_verify_result.value() == VerifyResultStep::Result::Matched);
        } else {
            emit cancelled(m_request.req_id);
        }
    }
    StepModal::done(result);
}

auto SigningFlow::signRows() const -> std::optional<QList<FlowRow>> {
    auto plan = m_module->rust()->signing_plan(rust::String(m_psbt.toStdString()));
    if (!plan.is_ok) {
        qWarning() << "keys: signing plan:" << KeysModule::toQt(plan.error);
        return std::nullopt;
    }
    QList<FlowRow> rows;
    for (const auto &candidate : plan.candidates) {
        rows.append(signRow(candidate));
    }
    return rows;
}

auto SigningFlow::signRow(const SigningCandidate &candidate) const -> FlowRow {
    FlowRow row;
    row.fingerprint = KeysModule::toQt(candidate.fingerprint);
    row.alias = KeysModule::toQt(candidate.alias);
    row.route = candidate.route;
    row.device_id = KeysModule::toQt(candidate.device_id);
    if (m_signed.contains(row.fingerprint)) {
        row.status = SignerStatus::Signed;
    } else if (candidate.can_sign) {
        row.status = candidate.must_sign_first ? SignerStatus::First : SignerStatus::Ready;
    } else {
        row.status = unusableStatus(candidate.unusable);
    }

    if (row.usable() && row.route == KeyRouteKind::Usb) {
        auto device = liveDeviceById(row.device_id);
        if (device.has_value()) {
            row.device = deviceName(*device->signer);
        }
    } else if (row.status == SignerStatus::Disconnected) {
        row.route = KeyRouteKind::Usb;
        row.device = rememberedDevice(row.fingerprint);
    }
    return row;
}

auto SigningFlow::verifyRows() const -> QList<FlowRow> {
    QList<FlowRow> rows;
    auto keys =
        m_module->rust()->keys_for_descriptor(rust::String(m_request.descriptor.toStdString()));
    for (const auto &info : keys) {
        FlowRow row;
        row.fingerprint = KeysModule::toQt(info.fingerprint);
        row.alias = KeysModule::toQt(info.alias);
        // Only a device with a screen can check an address: a connected USB device, or
        // one over QR codes.
        for (const auto &route : info.routes) {
            if (route == KeyRouteKind::Usb) {
                auto device = liveDevice(row.fingerprint);
                if (device.has_value()) {
                    row.route = KeyRouteKind::Usb;
                    row.device_id = device->instanceId;
                    row.device = deviceName(*device->signer);
                    row.status = SignerStatus::Ready;
                    break;
                }
            }
            if (route == KeyRouteKind::Qr) {
                row.route = KeyRouteKind::Qr;
                row.status = SignerStatus::Ready;
                break;
            }
        }
        if (!row.usable()) {
            bool hasUsb = std::ranges::find(info.routes, KeyRouteKind::Usb) != info.routes.end();
            row.status = hasUsb ? SignerStatus::Disconnected : SignerStatus::NoRoute;
            row.route = hasUsb ? KeyRouteKind::Usb : KeyRouteKind::Unknown;
            row.device = hasUsb ? rememberedDevice(row.fingerprint) : QString();
        }
        rows.append(row);
    }
    return rows;
}

auto SigningFlow::fetchRow() const -> FlowRow {
    FlowRow row;
    row.fingerprint = m_request.fingerprint;
    row.status = SignerStatus::Ready;
    for (const auto &info : m_module->keys()) {
        if (!sameFingerprint(KeysModule::toQt(info.fingerprint), m_request.fingerprint)) {
            continue;
        }
        row.alias = KeysModule::toQt(info.alias);
        row.route = KeysModule::descriptorRoute(info);
    }
    return row;
}

auto SigningFlow::otherDevices() const -> QList<OtherDevice> {
    QList<OtherDevice> others;
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer == nullptr) {
            continue;
        }
        QString fingerprint = entry.signer->fingerprint();
        bool known = std::ranges::any_of(m_rows, [&fingerprint](const FlowRow &row) {
            return sameFingerprint(row.fingerprint, fingerprint);
        });
        if (!known) {
            others.append({.name = deviceName(*entry.signer), .fingerprint = fingerprint});
        }
    }
    return others;
}

auto SigningFlow::rememberedDevice(const QString &fingerprint) const -> QString {
    auto devices = m_module->rust()->key_devices(rust::String(fingerprint.toStdString()));
    if (devices.empty()) {
        return QString();
    }
    return deviceName(KeysModule::toQt(devices.front().kind),
                      KeysModule::toQt(devices.front().model));
}

auto SigningFlow::currentDeviceLabel() const -> QString {
    if (!m_current->device.isEmpty()) {
        return m_current->device;
    }
    if (!m_current->alias.isEmpty()) {
        return m_current->alias;
    }
    return TR("flow-the-device");
}

auto SigningFlow::hasPicker() const -> bool {
    return m_request.context != Context::Fetch;
}

void SigningFlow::showPicker() {
    stopRoute();
    m_current.reset();
    bool verify = m_request.context == Context::Verify;
    m_picker->setRows(m_copy.pickerLead, m_rows, otherDevices(),
                      verify ? TR("flow-no-signer-verify") : TR("flow-no-signer-sign"),
                      verify ? TR("flow-other-device-other") : TR("flow-other-device-sign"));
    showStep(m_picker_page);
}

void SigningFlow::onRowPicked(int index) {
    if (index < 0 || index >= m_rows.size() || !m_rows.at(index).usable()) {
        return;
    }
    m_current = m_rows.at(index);
    startRoute();
}

void SigningFlow::startRoute() {
    stopRoute();
    m_verify_result.reset();
    switch (m_current->route) {
    case KeyRouteKind::Usb:
        startUsb();
        return;
    case KeyRouteKind::Hot:
        startHot();
        return;
    case KeyRouteKind::SdCard:
        startSd();
        return;
    case KeyRouteKind::Qr:
        startQr();
        return;
    case KeyRouteKind::Unknown:
    default:
        fail(TR("keys-route-unavailable"));
        return;
    }
}

void SigningFlow::stopRoute() {
    releaseDevice();
    m_hot_req = 0;
    m_sd_req = 0;
    m_module->qrRoute()->stop();
    m_qr->stop();
    m_camera->stop();
}

// ===== USB =====

void SigningFlow::startUsb() {
    showStep(m_usb_page);
    // A device that came back after an unplug has a new instance id.
    auto device = liveDeviceById(m_current->device_id);
    if (!device.has_value()) {
        device = liveDevice(m_current->fingerprint);
    }
    if (!device.has_value()) {
        m_usb->showError(UsbStep::Error::Disconnected, currentDeviceLabel());
        refit();
        return;
    }
    m_device = device->signer;
    m_device_instance = device->instanceId;

    QString address;
    switch (m_request.context) {
    case Context::Sign: {
        auto payload = ::usb_sign_payload(rust::String(m_psbt.toStdString()));
        if (!payload.is_ok) {
            fail(KeysModule::toQt(payload.error));
            return;
        }
        connect(m_device, &ISigner::signed_, this, &SigningFlow::onDeviceSigned, qontrol::UNIQUE);
        connect(m_device, &ISigner::error, this, &SigningFlow::onDeviceError, qontrol::UNIQUE);
        m_device_req = m_device->signWithDescriptor(KeysModule::toQt(payload.psbt_base64),
                                                    m_request.descriptor);
        break;
    }
    case Context::Verify:
        connect(m_device, &ISigner::addressDisplayed, this, &SigningFlow::onDeviceAddressDisplayed,
                qontrol::UNIQUE);
        connect(m_device, &ISigner::error, this, &SigningFlow::onDeviceError, qontrol::UNIQUE);
        m_device_req =
            m_device->displayAddress(m_request.descriptor, m_request.path, m_request.address);
        address = m_request.address;
        break;
    case Context::Fetch:
        // A descriptor only comes back over QR codes or from a hot key.
        fail(TR("keys-sp-descriptor-unavailable"));
        return;
    }
    m_usb->showWaiting(m_copy.usbTitle, m_copy.usbWait.arg(currentDeviceLabel()), address);
    refit();
}

void SigningFlow::releaseDevice() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
    m_device = nullptr;
    m_device_instance.clear();
    m_device_req = 0;
}

void SigningFlow::onDeviceSigned(ReqId req_id, const QString &psbt_base64) {
    if (m_device_req == 0 || req_id != m_device_req) {
        return;
    }
    releaseDevice();
    auto merged = ::usb_merge_signed(rust::String(m_psbt.toStdString()),
                                     rust::String(psbt_base64.toStdString()));
    if (!merged.is_ok) {
        fail(KeysModule::toQt(merged.error));
        return;
    }
    rowSigned(KeysModule::toQt(merged.psbt_v2_hex));
}

void SigningFlow::onDeviceAddressDisplayed(ReqId req_id, [[maybe_unused]] const QString &address) {
    if (m_device_req == 0 || req_id != m_device_req) {
        return;
    }
    releaseDevice();
    showVerifyResult(VerifyResultStep::Result::Matched);
}

void SigningFlow::onDeviceError(std::optional<ReqId> req_id, const QString &message) {
    if (m_device_req == 0 || !req_id.has_value() || req_id.value() != m_device_req) {
        return;
    }
    releaseDevice();
    qWarning() << "keys: device:" << message;
    auto error = usbErrorFor(message);
    // Refusing to confirm an address is the device's answer that it does not match.
    if (m_request.context == Context::Verify && error == UsbStep::Error::Rejected) {
        showVerifyResult(VerifyResultStep::Result::Mismatch);
        return;
    }
    m_usb->showError(error, currentDeviceLabel());
    refit();
}

void SigningFlow::onInstanceRemoved(const QString &id) {
    if (m_device_req == 0 || id != m_device_instance) {
        return;
    }
    releaseDevice();
    m_usb->showError(UsbStep::Error::Disconnected, currentDeviceLabel());
    refit();
}

void SigningFlow::onUsbBack() {
    showPicker();
}

void SigningFlow::onUsbRetry() {
    startUsb();
}

// ===== Hot key =====

void SigningFlow::startHot() {
    m_password->setPrompt(m_copy.hotPrompt.arg(m_current->alias));
    showStep(m_password_page);
    m_password->reset();
}

void SigningFlow::onPasswordEntered(QString password) {
    m_hot_req = m_module->nextReqId();
    m_busy->setTitle(m_copy.hotBusy);
    m_busy->setSubtitle(TR("flow-hot-busy-with").arg(m_current->alias));
    showStep(m_busy_page);
    switch (m_request.context) {
    case Context::Sign:
        m_module->signHot(m_hot_req, m_current->fingerprint, password, m_psbt);
        break;
    case Context::Fetch:
        m_module->fetchHotSpDescriptor(m_hot_req, m_current->fingerprint, password);
        break;
    case Context::Verify:
        // A hot key has no screen: verify rows never pick it.
        break;
    }
    password.fill(QChar());
    password.clear();
}

void SigningFlow::onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex,
                              const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    if (is_ok) {
        rowSigned(psbt_v2_hex);
        return;
    }
    if (wrong_password) {
        showStep(m_password_page);
        m_password->reset();
        m_password->showError(TR("flow-password-wrong"));
        refit();
        return;
    }
    fail(error);
}

void SigningFlow::onHotDescriptorFetched(ReqId req_id, bool is_ok, const QString &descriptor,
                                         const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    if (is_ok) {
        deliverDescriptor(descriptor);
        return;
    }
    if (wrong_password) {
        showStep(m_password_page);
        m_password->reset();
        m_password->showError(TR("flow-password-wrong"));
        refit();
        return;
    }
    fail(error);
}

// ===== SD card =====

void SigningFlow::startSd() {
    m_sd->setPhase(SdStep::Phase::Save);
    showStep(m_sd_page);
}

void SigningFlow::onSdSaveClicked() {
    QDir folder(m_sd_dir.isEmpty() ? QDir::homePath() : m_sd_dir);
    QString path = QFileDialog::getSaveFileName(
        this, TR("keys-sd-save-title"), folder.filePath(m_request.wallet_name + UNSIGNED_SUFFIX),
        TR("keys-sd-filter"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += PSBT_EXTENSION;
    }
    m_sd_dir = QFileInfo(path).absolutePath();
    m_sd_req = m_module->nextReqId();
    m_module->saveSdPsbt(m_sd_req, path, m_psbt);
}

void SigningFlow::onSdSaved(ReqId req_id, bool is_ok, const QString &error) {
    if (m_sd_req == 0 || req_id != m_sd_req) {
        return;
    }
    m_sd_req = 0;
    if (!is_ok) {
        qWarning() << "keys: sd save:" << error;
        m_sd->showWriteError();
        refit();
        return;
    }
    m_sd->setPhase(SdStep::Phase::Load);
    refit();
}

void SigningFlow::onSdLoadClicked() {
    QDir folder(m_sd_dir.isEmpty() ? QDir::homePath() : m_sd_dir);
    QString path = QFileDialog::getOpenFileName(
        this, TR("keys-sd-load-title"), folder.filePath(m_request.wallet_name + SIGNED_SUFFIX),
        TR("keys-sd-filter"));
    if (path.isEmpty()) {
        return;
    }
    m_sd_dir = QFileInfo(path).absolutePath();
    m_sd_req = m_module->nextReqId();
    m_module->loadSdPsbt(m_sd_req, path);
}

void SigningFlow::onSdLoaded(ReqId req_id, bool is_ok, const QString &psbt_hex,
                             const QString &error) {
    if (m_sd_req == 0 || req_id != m_sd_req) {
        return;
    }
    m_sd_req = 0;
    // The unsigned file read back, or a file this wallet cannot plan, is not the
    // answer the device wrote.
    bool readable = is_ok && psbt_hex != m_psbt &&
                    m_module->rust()->signing_plan(rust::String(psbt_hex.toStdString())).is_ok;
    if (!readable) {
        qWarning() << "keys: sd load:" << error;
        m_sd->showReadError();
        refit();
        return;
    }
    rowSigned(psbt_hex);
}

// ===== QR code =====

void SigningFlow::startQr() {
    showStep(m_qr_page);
    auto *qrRoute = m_module->qrRoute();
    switch (m_request.context) {
    case Context::Sign:
        qrRoute->startSign(m_qr, m_psbt, m_request.descriptor);
        return;
    case Context::Verify:
        qrRoute->startVerify(m_qr, m_request.descriptor, m_request.path, m_request.address);
        return;
    case Context::Fetch:
        qrRoute->startSpDescriptor(m_qr);
        return;
    }
}

void SigningFlow::onQrNext() {
    m_qr->stop();
    showStep(m_camera_page);
    m_camera->start();
}

void SigningFlow::onCameraBack() {
    m_camera->stop();
    showStep(m_qr_page);
    m_qr->restart();
}

void SigningFlow::onQrSigned(const QString &psbt_v2_hex) {
    m_camera->stop();
    rowSigned(psbt_v2_hex);
}

void SigningFlow::onQrVerified(bool matched) {
    m_camera->stop();
    showVerifyResult(matched ? VerifyResultStep::Result::Matched
                             : VerifyResultStep::Result::Mismatch);
}

void SigningFlow::onQrDescriptor(const QString &descriptor) {
    m_camera->stop();
    deliverDescriptor(descriptor);
}

void SigningFlow::onQrScanFailed(QrErrorKind kind, const QString &message) {
    m_camera->showScanError(kind, message);
    refit();
}

void SigningFlow::onQrFailed(const QString &message) {
    m_camera->stop();
    if (m_request.context == Context::Verify) {
        qWarning() << "keys: qr request:" << message;
        showVerifyResult(VerifyResultStep::Result::Error);
        return;
    }
    fail(message);
}

// ===== Endings =====

void SigningFlow::rowSigned(const QString &psbt_v2_hex) {
    m_psbt = psbt_v2_hex;
    m_signed.insert(m_current->fingerprint);
    auto rows = signRows();
    if (!rows.has_value()) {
        fail(TR("flow-plan-failed"));
        return;
    }
    m_rows = rows.value();
    bool pending = std::ranges::any_of(m_rows, [](const FlowRow &row) {
        return row.usable() || row.status == SignerStatus::Waiting;
    });
    if (pending) {
        showPicker();
        return;
    }
    m_answered = true;
    emit psbtSigned(m_request.req_id, m_psbt);
    accept();
}

void SigningFlow::showVerifyResult(VerifyResultStep::Result result) {
    m_verify_result = result;
    m_result->setResult(result, currentDeviceLabel());
    showStep(m_result_page);
}

void SigningFlow::onVerifyRetry() {
    startRoute();
}

void SigningFlow::deliverDescriptor(const QString &descriptor) {
    // Only a descriptor carrying the asked key's origin is handed over: never one for
    // another key, never one repaired here.
    auto validated = ::validate_sp_descriptor_origin(rust::String(descriptor.toStdString()));
    if (!validated.is_ok ||
        !sameFingerprint(KeysModule::toQt(validated.fingerprint), m_request.fingerprint)) {
        fail(TR("keys-sp-descriptor-no-origin"));
        return;
    }
    m_answered = true;
    emit descriptorFetched(m_request.req_id, KeysModule::toQt(validated.descriptor),
                           KeysModule::toQt(validated.fingerprint));
    accept();
}

void SigningFlow::fail(const QString &message) {
    m_answered = true;
    emit failed(m_request.req_id, message);
    reject();
}

void SigningFlow::refit() {
    showStep(currentStep());
}

} // namespace keys::flow
