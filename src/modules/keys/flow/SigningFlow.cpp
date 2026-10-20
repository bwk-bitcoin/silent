#include "SigningFlow.h"

#include "signingflow/HotRouteFlow.h"
#include "signingflow/LiveDevice.h"
#include "signingflow/QrRouteFlow.h"
#include "signingflow/SdRouteFlow.h"
#include "signingflow/UsbRouteFlow.h"

#include "catalog/ClearRows.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/feedback/EmptyState.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "theme/Icon.h"

#include <QDebug>
#include <Qontrol>
#include <QThread>

#include <algorithm>
#include <utility>

#include <common.h>

namespace keys::flow {

namespace {

constexpr const char *ROW_INDEX = "flowRowIndex";

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

// A Verify picker's key with nothing to show the address on reads No Screen.
auto statusPill(SignerStatus status, Context context) -> catalog::Badge * {
    if (context == Context::Verify && status == SignerStatus::NoRoute) {
        return catalog::pillAddrNoScreen();
    }
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

// The row's line under its title. A Verify picker speaks of showing the address,
// never of signing.
auto statusSub(const FlowRow &row, Context context) -> QString {
    if (context == Context::Verify) {
        switch (row.status) {
        case SignerStatus::Ready:
            return TR("flow-verify-ready-sub").arg(catalog::routeLabel(row.route));
        case SignerStatus::NoRoute:
            return row.route == KeyRouteKind::Hot ? TR("flow-verify-hot-sub")
                                                  : TR("flow-verify-no-route-sub");
        case SignerStatus::NoHot:
        case SignerStatus::First:
        case SignerStatus::Signed:
        case SignerStatus::Waiting:
        case SignerStatus::Disconnected:
        case SignerStatus::Unregistered:
            break;
        }
    }
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
    view();
    connect(m_module, &KeysModule::liveDevicesChanged, this, &SigningFlow::onDevicesChanged,
            qontrol::UNIQUE);
}

void SigningFlow::begin() {
    if (m_request.context == Context::Sign && !m_nonce_loaded) {
        QStringList fingerprints;
        for (const auto &key : m_module->keys()) {
            fingerprints.append(KeysModule::toQt(key.fingerprint));
        }
        auto *thread = QThread::create([fingerprints] {
            for (const auto &fingerprint : fingerprints) {
                auto result = ::ledger_nonce_refresh(fingerprint.toStdString());
                if (!result.is_ok) {
                    qWarning() << "Ledger nonce cache:" << KeysModule::toQt(result.error);
                }
            }
        });
        connect(thread, &QThread::finished, this, &SigningFlow::onNonceSnapshotReady, qontrol::UNIQUE);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
        thread->start();
        return;
    }
    switch (m_request.context) {
    case Context::Sign: {
        auto rows = signRows();
        if (!rows.has_value()) {
            onFailed(TR("flow-plan-failed"));
            return;
        }
        m_rows = rows.value();
        if (!m_request.fingerprint.isEmpty()) {
            startRequestedRow();
            return;
        }
        showPicker();
        return;
    }
    case Context::Verify:
        m_rows = verifyRows();
        if (!m_request.fingerprint.isEmpty()) {
            startRequestedRow();
            return;
        }
        showPicker();
        return;
    case Context::Fetch:
    case Context::FetchXpub:
        m_current = fetchRow();
        startRoute();
        return;
    }
}

auto SigningFlow::answered() const -> bool {
    return m_answered;
}

void SigningFlow::done(int result) {
    closeFlow();
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
}

void SigningFlow::view() {
    m_picker_step = viewPicker();
    hidePicker();
}

// ===== Picker view =====

void SigningFlow::initPicker() {
    m_picker_column = new qontrol::Column;
}

auto SigningFlow::viewPicker() -> QWidget * {
    return (new qontrol::Column)->push(m_picker_column);
}

void SigningFlow::showPicker() {
    closeFlow();
    m_current.reset();
    bool verify = m_request.context == Context::Verify;

    // clear() also resets the layout spacing
    catalog::clearRows(m_picker_column);
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
    if (m_signed.contains(row.fingerprint) || candidate.musig_done) {
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
    // A silent payment address is only checked over QR codes: no USB device protocol
    // supports it.
    bool silentPayment = ::validate_sp_descriptor(rust::String(m_request.descriptor.toStdString()));
    for (const auto &info : keys) {
        FlowRow row;
        row.fingerprint = KeysModule::toQt(info.fingerprint);
        row.alias = KeysModule::toQt(info.alias);
        // Only a device with a screen can check an address: a connected USB device, or
        // one over QR codes.
        for (const auto &route : info.routes) {
            if (route == KeyRouteKind::Usb && !silentPayment) {
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
            bool hasUsb = !silentPayment &&
                          std::ranges::find(info.routes, KeyRouteKind::Usb) != info.routes.end();
            bool hasHot = std::ranges::find(info.routes, KeyRouteKind::Hot) != info.routes.end();
            row.status = hasUsb ? SignerStatus::Disconnected : SignerStatus::NoRoute;
            // A hot key keeps its route: its row says it has no screen.
            row.route =
                hasUsb ? KeyRouteKind::Usb : (hasHot ? KeyRouteKind::Hot : KeyRouteKind::Unknown);
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
        row.route = m_request.context == Context::FetchXpub ? KeysModule::xpubRoute(info)
                                                            : KeysModule::descriptorRoute(info);
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
    selectRow->setSub(statusSub(row, m_request.context));
    selectRow->setMeta(row.fingerprint);
    selectRow->setStatus(statusPill(row.status, m_request.context));
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

// ===== Shared =====

void SigningFlow::startRoute() {
    closeFlow();
    m_verify_result.reset();
    switch (m_current->route) {
    case KeyRouteKind::Usb:
        m_flow = new UsbRouteFlow(m_module, m_request, m_current.value(), m_psbt);
        break;
    case KeyRouteKind::Hot:
        m_flow = new HotRouteFlow(m_module, m_request, m_current.value(), m_psbt);
        break;
    case KeyRouteKind::SdCard:
        m_flow = new SdRouteFlow(m_module, m_request, m_current.value(), m_psbt);
        break;
    case KeyRouteKind::Qr:
        m_flow = new QrRouteFlow(m_module, m_request, m_current.value(), m_psbt);
        break;
    case KeyRouteKind::Unknown:
    default:
        onFailed(TR("keys-route-unavailable"));
        return;
    }
    connect(m_flow, &SigningRouteFlow::viewChanged, this, &SigningFlow::onFlowViewChanged,
            qontrol::UNIQUE);
    connect(m_flow, &SigningRouteFlow::back, this, &SigningFlow::onFlowBack, qontrol::UNIQUE);
    connect(m_flow, &SigningRouteFlow::psbtSigned, this, &SigningFlow::onPsbtSigned,
            qontrol::UNIQUE);
    connect(m_flow, &SigningRouteFlow::verifyResultChanged, this,
            &SigningFlow::onVerifyResultChanged, qontrol::UNIQUE);
    connect(m_flow, &SigningRouteFlow::descriptorFetched, this, &SigningFlow::onDescriptorFetched,
            qontrol::UNIQUE);
    connect(m_flow, &SigningRouteFlow::xpubFetched, this, &SigningFlow::onXpubFetched,
            qontrol::UNIQUE);
    connect(m_flow, &SigningRouteFlow::failed, this, &SigningFlow::onFailed, qontrol::UNIQUE);
    connect(m_flow, &SigningRouteFlow::cancelled, this, &QDialog::reject, qontrol::UNIQUE);
    setPage(m_flow);
    m_flow->start();
}

void SigningFlow::startRequestedRow() {
    auto found = std::ranges::find_if(m_rows, [this](const FlowRow &row) {
        return sameFingerprint(row.fingerprint, m_request.fingerprint);
    });
    if (found == m_rows.end()) {
        onFailed(m_request.context == Context::Verify ? TR("flow-verify-key-not-needed")
                                                      : TR("flow-signer-not-needed"));
        return;
    }
    if (!found->usable()) {
        onFailed(statusSub(*found, m_request.context));
        return;
    }
    m_current = *found;
    startRoute();
}

void SigningFlow::closeFlow() {
    if (m_flow == nullptr) {
        return;
    }
    // Cut off first: an answer the route still gets must not reach a request already
    // answered. Deleted later: the route may still be in the slot that emitted.
    disconnect(m_flow, nullptr, this, nullptr);
    m_flow->deleteLater();
    m_flow = nullptr;
}

// ===== Slots =====

void SigningFlow::onNonceSnapshotReady() {
    m_nonce_loaded = true;
    if (!m_answered) {
        begin();
    }
}

void SigningFlow::onDevicesChanged() {
    // Only the picker lists devices: a route on display keeps its own device.
    if (m_flow != nullptr || m_answered || (m_request.context == Context::Sign && !m_nonce_loaded)) {
        return;
    }
    switch (m_request.context) {
    case Context::Sign: {
        auto rows = signRows();
        if (!rows.has_value()) {
            return;
        }
        m_rows = rows.value();
        break;
    }
    case Context::Verify:
        m_rows = verifyRows();
        break;
    case Context::Fetch:
    case Context::FetchXpub:
        return;
    }
    showPicker();
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

void SigningFlow::onFlowViewChanged(const QString &title, ModalWidth width) {
    setWindowTitle(title);
    setWidth(width);
}

void SigningFlow::onFlowBack() {
    showPicker();
}

void SigningFlow::onPsbtSigned(const QString &psbt_v2_hex) {
    m_psbt = psbt_v2_hex;
    m_signed.insert(m_current->fingerprint);
    auto rows = signRows();
    if (!rows.has_value()) {
        onFailed(TR("flow-plan-failed"));
        return;
    }
    m_rows = rows.value();
    // A request for one key is done once that key has signed.
    bool pending =
        m_request.fingerprint.isEmpty() && std::ranges::any_of(m_rows, [](const FlowRow &row) {
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

void SigningFlow::onVerifyResultChanged(std::optional<SigningFlow::VerifyResult> result) {
    m_verify_result = result;
}

void SigningFlow::onDescriptorFetched(const QString &descriptor, const QList<qr::QrXpub> &xpubs) {
    // Only a descriptor carrying the asked key's origin is handed over: never one for
    // another key, never one repaired here.
    auto validated = ::validate_sp_descriptor_origin(rust::String(descriptor.toStdString()));
    if (!validated.is_ok ||
        !sameFingerprint(KeysModule::toQt(validated.fingerprint), m_request.fingerprint)) {
        onFailed(TR("keys-sp-descriptor-no-origin"));
        return;
    }
    m_answered = true;
    emit descriptorFetched(m_request.req_id, KeysModule::toQt(validated.descriptor),
                           KeysModule::toQt(validated.fingerprint), xpubs);
    accept();
}

void SigningFlow::onXpubFetched(const QString &xpub) {
    m_answered = true;
    emit xpubFetched(m_request.req_id, xpub, m_request.fingerprint, m_request.path);
    accept();
}

void SigningFlow::onFailed(const QString &message, plugin::SignError kind) {
    m_answered = true;
    emit failed(m_request.req_id, message, kind);
    reject();
}

// ===== SigningRouteFlow =====

SigningRouteFlow::SigningRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row,
                                   QString psbt)
    : m_module(module),
      m_request(std::move(request)),
      m_copy(flowCopy(m_request.context)),
      m_row(std::move(row)),
      m_psbt(std::move(psbt)) {
    m_slots = new catalog::ViewSlots;
    (new qontrol::Column)->push(m_slots)->into(this);
}

auto SigningRouteFlow::viewSlots() const -> catalog::ViewSlots * {
    return m_slots;
}

auto SigningRouteFlow::module() const -> KeysModule * {
    return m_module;
}

auto SigningRouteFlow::request() const -> const SigningFlow::Request & {
    return m_request;
}

auto SigningRouteFlow::copy() const -> const FlowCopy & {
    return m_copy;
}

auto SigningRouteFlow::row() const -> const FlowRow & {
    return m_row;
}

auto SigningRouteFlow::psbt() const -> const QString & {
    return m_psbt;
}

auto SigningRouteFlow::hasPicker() const -> bool {
    // Fetch and FetchXpub always name their key, and so does a Sign request for one key.
    return m_request.fingerprint.isEmpty();
}

auto SigningRouteFlow::deviceLabel() const -> QString {
    if (!m_row.device.isEmpty()) {
        return m_row.device;
    }
    if (!m_row.alias.isEmpty()) {
        return m_row.alias;
    }
    return TR("flow-the-device");
}

} // namespace keys::flow
