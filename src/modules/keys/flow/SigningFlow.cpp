#include "SigningFlow.h"

#include "CameraStep.h"
#include "QrDisplayStep.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/inputs/Input.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/QrRoute.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <Qontrol>

#include <algorithm>
#include <utility>

#include <common.h>

namespace keys::flow {

namespace {

const QString UNSIGNED_SUFFIX = QStringLiteral("-unsigned.psbt");
const QString SIGNED_SUFFIX = QStringLiteral("-signed.psbt");
const QString PSBT_EXTENSION = QStringLiteral(".psbt");
constexpr const char *ROW_INDEX = "flowRowIndex";

// bwk-hwi folds its error kinds into text at the device service boundary, so the kind
// is read back from its own messages (bwk hwi Error's Display).
auto usbErrorFor(const QString &message) -> SigningFlow::UsbError {
    if (message.contains(QStringLiteral("User refused"), Qt::CaseInsensitive)) {
        return SigningFlow::UsbError::Rejected;
    }
    if (message.contains(QStringLiteral("Device disconnected"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("Device not found"), Qt::CaseInsensitive)) {
        return SigningFlow::UsbError::Disconnected;
    }
    return SigningFlow::UsbError::Failed;
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

auto statusPill(SignerStatus status) -> catalog::Badge * {
    switch (status) {
    case SignerStatus::First:
        return catalog::pillSignerFirst();
    case SignerStatus::Ready:
        return catalog::pillSignerReady();
    case SignerStatus::Signed:
        return catalog::pillSignerSigned();
    case SignerStatus::Waiting:
        return catalog::pillSignerWaiting();
    case SignerStatus::Disconnected:
        return catalog::pillSignerDisconnected();
    case SignerStatus::NoHot:
        return catalog::pillSignerNoHot();
    case SignerStatus::NoRoute:
        return catalog::pillSignerNoRoute();
    case SignerStatus::Unregistered:
        return catalog::pillSignerUnregistered();
    }
    return catalog::pillSignerNoRoute();
}

auto statusSub(const FlowRow &row) -> QString {
    switch (row.status) {
    case SignerStatus::First:
        return TR("flow-signer-first-sub");
    case SignerStatus::Ready:
        return TR("flow-signer-ready-sub").arg(catalog::routeLabel(row.route));
    case SignerStatus::Signed:
        return TR("flow-signer-signed-sub");
    case SignerStatus::Waiting:
        return TR("flow-signer-waiting-sub");
    case SignerStatus::Disconnected:
        return TR("flow-signer-disconnected-sub")
            .arg(row.device.isEmpty() ? TR("flow-the-device") : row.device);
    case SignerStatus::NoHot:
        return TR("flow-signer-no-hot-sub");
    case SignerStatus::NoRoute:
        return TR("flow-signer-no-route-sub");
    case SignerStatus::Unregistered:
        return TR("flow-signer-unregistered-sub");
    }
    return QString();
}

auto otherRow(const OtherDevice &device, const QString &sub) -> catalog::SelectRow * {
    auto *selectRow = new catalog::SelectRow;
    selectRow->setIcon(icon::usb);
    selectRow->setTitle(device.name);
    selectRow->setSub(sub);
    selectRow->setMeta(device.fingerprint);
    selectRow->setClickable(false);
    selectRow->setEnabled(false);
    return selectRow;
}

// A file name inside a sentence, set in the mono face.
auto monoFileName(const QString &name) -> QString {
    return QStringLiteral("<span style=\"font-family: '%1'\">%2</span>")
        .arg(font::MONO, name.toHtmlEscaped());
}

} // namespace

auto FlowRow::usable() const -> bool {
    return status == SignerStatus::First || status == SignerStatus::Ready;
}

SigningFlow::SigningFlow(KeysModule *module, Request request)
    : m_module(module),
      m_request(std::move(request)),
      m_copy(flowCopy(m_request.context)),
      m_psbt(m_request.psbt_v2_hex) {
    init();
    doConnect();
    view();
}

SigningFlow::~SigningFlow() {
    m_password_input->clear();
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
        bool checked =
            m_verify_result.has_value() && m_verify_result.value() != VerifyResult::Error;
        if (checked) {
            emit addressVerified(m_request.req_id,
                                 m_verify_result.value() == VerifyResult::Matched);
        } else {
            emit cancelled(m_request.req_id);
        }
    }
    catalog::Modal::done(result);
}

void SigningFlow::init() {
    initPicker();
    initUsb();
    initSd();
    initPassword();
    initBusy();
    initQr();
    initVerifyResult();
}

void SigningFlow::doConnect() {
    connectUsb();
    connectSd();
    connectPassword();
    connectQr();
    connectVerifyResult();
}

void SigningFlow::view() {
    m_picker_step = viewPicker();
    m_usb_step = viewUsb();
    m_sd_step = viewSd();
    m_password_step = viewPassword();
    m_result_step = viewVerifyResult();

    hidePicker();
    hideUsb();
    hidePassword();
    hideBusy();
    hideSd();
    hideQr();
    hideVerifyResult();
}

// ===== Picker view =====

void SigningFlow::initPicker() {
    m_picker_column = new qontrol::Column;
}

auto SigningFlow::viewPicker() -> QWidget * {
    return (new qontrol::Column)->push(m_picker_column);
}

void SigningFlow::showPicker() {
    m_view = View::Picker;
    stopRoute();
    m_current.reset();
    bool verify = m_request.context == Context::Verify;

    // clear() also resets the layout spacing
    m_picker_column->clear();
    m_picker_column->spacing(resolve(Spacing::M));
    // Each part added to the shown step is shown at once, or the step would be measured
    // without it until the next event loop pass.
    if (m_rows.isEmpty()) {
        auto *emptyState = new catalog::EmptyState(icon::key, TR("flow-no-signer-title"),
                                                   verify ? TR("flow-no-signer-verify")
                                                          : TR("flow-no-signer-sign"));
        m_picker_column->push(emptyState);
        emptyState->show();
    } else {
        if (!m_copy.pickerLead.isEmpty()) {
            auto *leadLabel = new catalog::Label(m_copy.pickerLead, catalog::LabelRole::Body);
            leadLabel->setTone(catalog::LabelTone::Secondary);
            leadLabel->setWordWrap(true);
            m_picker_column->push(leadLabel);
            leadLabel->show();
        }
        auto *rowsColumn = (new qontrol::Column)->spacing(resolve(Spacing::S));
        for (int index = 0; index < m_rows.size(); ++index) {
            rowsColumn->push(buildRow(m_rows.at(index), index));
        }
        m_picker_column->push(rowsColumn);
        rowsColumn->show();
    }
    auto others = otherDevices();
    if (!others.isEmpty()) {
        QString otherSub = verify ? TR("flow-other-device-other") : TR("flow-other-device-sign");
        auto *othersColumn =
            (new qontrol::Column)
                ->spacing(resolve(Spacing::S))
                ->push(new catalog::Label(TR("flow-other-devices"), catalog::LabelRole::Eyebrow));
        for (const auto &device : others) {
            othersColumn->push(otherRow(device, otherSub));
        }
        m_picker_column->push(othersColumn);
        othersColumn->show();
    }

    setWindowTitle(m_copy.title);
    setHeader(nullptr);
    setBody(m_picker_step);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

void SigningFlow::hidePicker() {
    keep(m_picker_step);
}

auto SigningFlow::hasPicker() const -> bool {
    return m_request.context != Context::Fetch;
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

auto SigningFlow::buildRow(const FlowRow &row, int index) const -> catalog::SelectRow * {
    auto *selectRow = new catalog::SelectRow;
    selectRow->setIcon(row.route != KeyRouteKind::Unknown ? catalog::routeIcon(row.route)
                                                          : icon::key);
    selectRow->setTitle(row.alias.isEmpty() ? TR("flow-unknown-key") : row.alias);
    selectRow->setSub(statusSub(row));
    selectRow->setMeta(row.fingerprint);
    selectRow->setStatus(statusPill(row.status));
    selectRow->setStatusWidth(resolve(PillWidth::L));
    if (row.usable()) {
        selectRow->setProperty(ROW_INDEX, index);
        connect(selectRow, &catalog::SelectRow::clicked, this, &SigningFlow::onRowClicked,
                qontrol::UNIQUE);
    } else {
        selectRow->setClickable(false);
        selectRow->setEnabled(row.status == SignerStatus::Signed);
    }
    return selectRow;
}

// ===== USB view =====

void SigningFlow::initUsb() {
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

void SigningFlow::connectUsb() {
    connect(m_usb_actions, &catalog::ModalActions::backClicked, this, &SigningFlow::onBackClicked,
            qontrol::UNIQUE);
    connect(m_usb_retry, &QPushButton::clicked, this, &SigningFlow::onNextClicked, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &SigningFlow::onInstanceRemoved, qontrol::UNIQUE);
}

auto SigningFlow::viewUsb() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_usb_status)
        ->push(m_usb_address_card);
}

void SigningFlow::showUsb() {
    m_view = View::Usb;
    setWindowTitle(m_copy.usbTitle);
    setHeader(nullptr);
    setBody(m_usb_step);
    setFooter(nullptr);
    setWidth(ModalWidth::S);
}

// The failure with Back to the other signers and Try Again under it.
void SigningFlow::showUsbError(UsbError error) {
    m_view = View::Usb;
    QString device = currentDeviceLabel();
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
    setWindowTitle(m_copy.usbTitle);
    setHeader(nullptr);
    setBody(m_usb_step);
    setFooter(m_usb_actions);
    setWidth(ModalWidth::S);
}

void SigningFlow::hideUsb() {
    keep(m_usb_step);
    keep(m_usb_actions);
}

void SigningFlow::startUsb() {
    // A device that came back after an unplug has a new instance id.
    auto device = liveDeviceById(m_current->device_id);
    if (!device.has_value()) {
        device = liveDevice(m_current->fingerprint);
    }
    if (!device.has_value()) {
        showUsbError(UsbError::Disconnected);
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
    m_usb_status->setSpinner();
    m_usb_status->setTitle(m_copy.usbTitle);
    m_usb_status->setSubtitle(m_copy.usbWait.arg(currentDeviceLabel()));
    m_usb_address->setAddress(address);
    m_usb_address_card->setVisible(!address.isEmpty());
    showUsb();
}

void SigningFlow::releaseDevice() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
    m_device = nullptr;
    m_device_instance.clear();
    m_device_req = 0;
}

// ===== Hot key password view =====

void SigningFlow::initPassword() {
    m_password_prompt = new catalog::Label(catalog::LabelRole::Body);
    m_password_prompt->setTone(catalog::LabelTone::Secondary);
    m_password_prompt->setWordWrap(true);
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_password_proceed = hotProceedButton(m_request.context);
    m_password_actions = new catalog::ModalActions;
    m_password_actions->addAction(m_password_proceed);
}

void SigningFlow::connectPassword() {
    connect(m_password_input, &QLineEdit::textEdited, this, &SigningFlow::onPasswordEdited,
            qontrol::UNIQUE);
    connect(m_password_input, &QLineEdit::returnPressed, this, &SigningFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_password_proceed, &QPushButton::clicked, this, &SigningFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotSigned, this, &SigningFlow::onHotSigned, qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotSpDescriptorFetched, this,
            &SigningFlow::onHotDescriptorFetched, qontrol::UNIQUE);
}

auto SigningFlow::viewPassword() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_password_prompt)
        ->push(new catalog::Field(TR("flow-password"), m_password_input))
        ->push(m_password_error);
}

// An empty field with the focus in it, and the error kept if one was just shown.
void SigningFlow::showPassword() {
    m_view = View::Password;
    m_password_input->clear();
    onPasswordEdited();
    setWindowTitle(m_copy.title);
    setHeader(nullptr);
    setBody(m_password_step);
    setFooter(m_password_actions);
    setWidth(ModalWidth::S);
    m_password_input->setFocus();
}

void SigningFlow::hidePassword() {
    keep(m_password_step);
    keep(m_password_actions);
}

void SigningFlow::startHot() {
    QString prompt = m_copy.hotPrompt.arg(m_current->alias);
    m_password_prompt->setText(prompt);
    m_password_prompt->setVisible(!prompt.isEmpty());
    m_password_error->setVisible(false);
    showPassword();
}

void SigningFlow::proceedPassword() {
    if (m_password_input->text().isEmpty()) {
        return;
    }
    QString password = m_password_input->text();
    m_password_input->clear();
    m_password_error->setVisible(false);
    m_hot_req = m_module->nextReqId();
    m_busy_step->setTitle(m_copy.hotBusy);
    m_busy_step->setSubtitle(TR("flow-hot-busy-with").arg(m_current->alias));
    showBusy();
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

// ===== Busy view =====

void SigningFlow::initBusy() {
    m_busy_step = new catalog::ModalStatus;
    m_busy_step->setSpinner();
    m_busy_step->setPadding(resolve(Spacing::M), resolve(Spacing::M));
}

void SigningFlow::showBusy() {
    m_view = View::Busy;
    setWindowTitle(m_copy.title);
    setHeader(nullptr);
    setBody(m_busy_step);
    setFooter(nullptr);
    setWidth(ModalWidth::S);
}

void SigningFlow::hideBusy() {
    keep(m_busy_step);
}

// ===== SD card view =====

void SigningFlow::initSd() {
    m_sd_save_row = new catalog::SelectRow;
    m_sd_save_row->setTitle(TR("flow-sd-save-title"));
    m_sd_save_row->setSub(
        TR("flow-sd-save-body").arg(monoFileName(m_request.wallet_name + UNSIGNED_SUFFIX)));
    m_sd_save_row->setClickable(false);
    m_sd_load_row = new catalog::SelectRow;
    m_sd_load_row->setTitle(TR("flow-sd-load-title"));
    m_sd_load_row->setSub(
        TR("flow-sd-load-body").arg(monoFileName(m_request.wallet_name + SIGNED_SUFFIX)));
    m_sd_load_row->setClickable(false);
    m_sd_write_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_sd_write_error->setTitle(TR("flow-sd-write-error-title"));
    m_sd_write_error->setText(TR("flow-sd-write-error-body"));
    m_sd_read_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_sd_read_error->setTitle(TR("flow-sd-read-error-title"));
    m_sd_read_error->setText(TR("flow-sd-read-error-body"));
    m_sd_save = catalog::btnSaveUnsignedPsbt();
    m_sd_save_retry = catalog::btnTryAgain();
    m_sd_load = catalog::btnLoadSignedPsbt();
    m_sd_load_retry = catalog::btnPickAnotherFile();
    m_sd_actions = new catalog::ModalActions;
    m_sd_actions->addAction(m_sd_save);
    m_sd_actions->addAction(m_sd_save_retry);
    m_sd_actions->addAction(m_sd_load);
    m_sd_actions->addAction(m_sd_load_retry);
}

void SigningFlow::connectSd() {
    connect(m_sd_save, &QPushButton::clicked, this, &SigningFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_sd_save_retry, &QPushButton::clicked, this, &SigningFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_sd_load, &QPushButton::clicked, this, &SigningFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_sd_load_retry, &QPushButton::clicked, this, &SigningFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::sdSaved, this, &SigningFlow::onSdSaved, qontrol::UNIQUE);
    connect(m_module, &KeysModule::sdLoaded, this, &SigningFlow::onSdLoaded, qontrol::UNIQUE);
}

auto SigningFlow::viewSd() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_sd_save_row)
        ->push(m_sd_load_row)
        ->push(m_sd_write_error)
        ->push(m_sd_read_error);
}

void SigningFlow::showSd() {
    m_view = View::Sd;
    refreshSd();
    setWindowTitle(m_copy.sdTitle);
    setHeader(nullptr);
    setBody(m_sd_step);
    setFooter(m_sd_actions);
    setWidth(ModalWidth::M);
}

void SigningFlow::hideSd() {
    keep(m_sd_step);
    keep(m_sd_actions);
}

void SigningFlow::startSd() {
    m_sd_phase = SdPhase::Save;
    m_sd_failure = SdFailure::None;
    showSd();
}

void SigningFlow::refreshSd() {
    bool saving = m_sd_phase == SdPhase::Save;
    m_sd_save_row->setIcon(saving ? icon::sdCard : icon::circleCheck);
    m_sd_save_row->setCurrent(saving);
    m_sd_save_row->setStatus(saving ? nullptr : catalog::pillSdStepDone());
    m_sd_load_row->setIcon(icon::sdCard);
    m_sd_load_row->setCurrent(!saving);
    m_sd_load_row->setEnabled(!saving);

    bool writeFailed = m_sd_failure == SdFailure::Write;
    bool readFailed = m_sd_failure == SdFailure::Read;
    m_sd_write_error->setVisible(writeFailed);
    m_sd_read_error->setVisible(readFailed);
    m_sd_save->setVisible(saving && !writeFailed);
    m_sd_save_retry->setVisible(saving && writeFailed);
    m_sd_load->setVisible(!saving && !readFailed);
    m_sd_load_retry->setVisible(!saving && readFailed);
}

void SigningFlow::saveSd() {
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

void SigningFlow::loadSd() {
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

// ===== QR code and camera views =====

void SigningFlow::initQr() {
    m_qr = new QrDisplayStep;
    m_qr->setHint(m_copy.qrHint);
    m_camera = new CameraStep(m_module);
    m_camera->setHint(m_copy.scanHint);
}

void SigningFlow::connectQr() {
    connect(m_qr, &QrDisplayStep::nextClicked, this, &SigningFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_camera, &CameraStep::backClicked, this, &SigningFlow::onBackClicked, qontrol::UNIQUE);
    auto *qrRoute = m_module->qrRoute();
    connect(qrRoute, &QrRoute::psbtSigned, this, &SigningFlow::onQrSigned, qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::addressVerified, this, &SigningFlow::onQrVerified, qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::descriptorDecoded, this, &SigningFlow::onQrDescriptor,
            qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::scanFailed, this, &SigningFlow::onQrScanFailed, qontrol::UNIQUE);
    connect(qrRoute, &QrRoute::failed, this, &SigningFlow::onQrFailed, qontrol::UNIQUE);
}

void SigningFlow::showQrDisplay() {
    m_view = View::QrDisplay;
    setWindowTitle(m_copy.qrTitle);
    setHeader(nullptr);
    setBody(m_qr);
    setFooter(m_qr->actions());
    setWidth(ModalWidth::XXL);
}

void SigningFlow::showCamera() {
    m_view = View::Camera;
    setWindowTitle(m_copy.scanTitle);
    setHeader(nullptr);
    setBody(m_camera);
    setFooter(m_camera->actions());
    setWidth(ModalWidth::XXL);
}

void SigningFlow::hideQr() {
    keep(m_qr);
    keep(m_qr->actions());
    keep(m_camera);
    keep(m_camera->actions());
}

void SigningFlow::startQr() {
    showQrDisplay();
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

// ===== Verify result view =====

void SigningFlow::initVerifyResult() {
    m_result_status = new catalog::ModalStatus;
    m_result_status->setPadding(resolve(Spacing::S), 0);
    m_result_address = new catalog::ChunkedAddress;
    m_result_address->setAddress(m_request.address);
    m_result_retry = catalog::btnTryAgain();
    m_result_actions = new catalog::ModalActions;
    m_result_actions->addAction(m_result_retry);
}

void SigningFlow::connectVerifyResult() {
    connect(m_result_retry, &QPushButton::clicked, this, &SigningFlow::onNextClicked,
            qontrol::UNIQUE);
}

auto SigningFlow::viewVerifyResult() -> QWidget * {
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setPad(catalog::Card::Pad::M);
    card->setContent(m_result_address);
    return (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_result_status)->push(card);
}

void SigningFlow::showVerifyResult(VerifyResult result) {
    m_view = View::VerifyResult;
    m_verify_result = result;
    QString device = currentDeviceLabel();
    switch (result) {
    case VerifyResult::Matched:
        m_result_status->setIcon(icon::circleCheck, IconColor::Success);
        m_result_status->setTitle(TR("flow-verify-matched-title"));
        m_result_status->setSubtitle(TR("flow-verify-matched-body").arg(device));
        break;
    case VerifyResult::Mismatch:
        m_result_status->setIcon(icon::alert, IconColor::Error);
        m_result_status->setTitle(TR("flow-verify-mismatch-title"));
        m_result_status->setSubtitle(TR("flow-verify-mismatch-body").arg(device));
        break;
    case VerifyResult::Error:
        m_result_status->setIcon(icon::circleX, IconColor::Muted);
        m_result_status->setTitle(TR("flow-verify-error-title"));
        m_result_status->setSubtitle(TR("flow-verify-error-body").arg(device));
        break;
    }
    setWindowTitle(m_copy.title);
    setHeader(nullptr);
    setBody(m_result_step);
    // A match needs no Try Again.
    setFooter(result == VerifyResult::Matched ? nullptr : m_result_actions);
    setWidth(ModalWidth::S);
}

void SigningFlow::hideVerifyResult() {
    keep(m_result_step);
    keep(m_result_actions);
}

// ===== Shared =====

auto SigningFlow::currentDeviceLabel() const -> QString {
    if (!m_current->device.isEmpty()) {
        return m_current->device;
    }
    if (!m_current->alias.isEmpty()) {
        return m_current->alias;
    }
    return TR("flow-the-device");
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

// ===== Slots =====

void SigningFlow::onBackClicked() {
    switch (m_view) {
    case View::Usb:
        showPicker();
        return;
    case View::Camera:
        m_camera->stop();
        showQrDisplay();
        m_qr->restart();
        return;
    case View::Picker:
    case View::Password:
    case View::Busy:
    case View::Sd:
    case View::QrDisplay:
    case View::VerifyResult:
        return;
    }
}

void SigningFlow::onNextClicked() {
    switch (m_view) {
    case View::Usb:
        startUsb();
        return;
    case View::Password:
        proceedPassword();
        return;
    case View::Sd:
        if (m_sd_phase == SdPhase::Save) {
            saveSd();
        } else {
            loadSd();
        }
        return;
    case View::QrDisplay:
        m_qr->stop();
        showCamera();
        m_camera->start();
        return;
    case View::VerifyResult:
        startRoute();
        return;
    case View::Picker:
    case View::Busy:
    case View::Camera:
        return;
    }
}

void SigningFlow::onRowClicked() {
    auto *row = sender();
    if (row == nullptr) {
        return;
    }
    int index = row->property(ROW_INDEX).toInt();
    if (index < 0 || index >= m_rows.size() || !m_rows.at(index).usable()) {
        return;
    }
    m_current = m_rows.at(index);
    startRoute();
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
    showVerifyResult(VerifyResult::Matched);
}

void SigningFlow::onDeviceError(std::optional<ReqId> req_id, const QString &message) {
    if (m_device_req == 0 || !req_id.has_value() || req_id.value() != m_device_req) {
        return;
    }
    releaseDevice();
    qWarning() << "keys: device:" << message;
    auto error = usbErrorFor(message);
    // Refusing to confirm an address is the device's answer that it does not match.
    if (m_request.context == Context::Verify && error == UsbError::Rejected) {
        showVerifyResult(VerifyResult::Mismatch);
        return;
    }
    showUsbError(error);
}

void SigningFlow::onInstanceRemoved(const QString &id) {
    if (m_device_req == 0 || id != m_device_instance) {
        return;
    }
    releaseDevice();
    showUsbError(UsbError::Disconnected);
}

void SigningFlow::onPasswordEdited() {
    bool empty = m_password_input->text().isEmpty();
    if (!empty) {
        m_password_error->setVisible(false);
    }
    m_password_proceed->setEnabled(!empty);
    m_password_actions->setReason(empty ? TR("flow-password-reason") : QString());
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
        m_password_error->setText(TR("flow-password-wrong"));
        m_password_error->setVisible(true);
        showPassword();
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
        m_password_error->setText(TR("flow-password-wrong"));
        m_password_error->setVisible(true);
        showPassword();
        return;
    }
    fail(error);
}

void SigningFlow::onSdSaved(ReqId req_id, bool is_ok, const QString &error) {
    if (m_sd_req == 0 || req_id != m_sd_req) {
        return;
    }
    m_sd_req = 0;
    if (!is_ok) {
        qWarning() << "keys: sd save:" << error;
        m_sd_failure = SdFailure::Write;
        showSd();
        return;
    }
    m_sd_phase = SdPhase::Load;
    m_sd_failure = SdFailure::None;
    showSd();
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
        m_sd_failure = SdFailure::Read;
        showSd();
        return;
    }
    rowSigned(psbt_hex);
}

void SigningFlow::onQrSigned(const QString &psbt_v2_hex) {
    m_camera->stop();
    rowSigned(psbt_v2_hex);
}

void SigningFlow::onQrVerified(bool matched) {
    m_camera->stop();
    showVerifyResult(matched ? VerifyResult::Matched : VerifyResult::Mismatch);
}

void SigningFlow::onQrDescriptor(const QString &descriptor) {
    m_camera->stop();
    deliverDescriptor(descriptor);
}

void SigningFlow::onQrScanFailed(QrErrorKind kind, const QString &message) {
    m_camera->showScanError(kind, message);
    adjustSize();
}

void SigningFlow::onQrFailed(const QString &message) {
    m_camera->stop();
    if (m_request.context == Context::Verify) {
        qWarning() << "keys: qr request:" << message;
        showVerifyResult(VerifyResult::Error);
        return;
    }
    fail(message);
}

} // namespace keys::flow
