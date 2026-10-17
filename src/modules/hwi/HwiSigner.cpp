#include "HwiSigner.h"

#include "HwiModule.h"

#include "interfaces/host.h"
#include "interfaces/keys.h"

#include <QDebug>
#include <QMap>

#include <common.h>
#include <silent.h>

namespace {
auto toState(HwiDeviceState state) -> plugin::SignerState {
    switch (state) {
    case HwiDeviceState::Supported:
        return plugin::SignerState::Ready;
    case HwiDeviceState::Locked:
        return plugin::SignerState::Locked;
    default:
        return plugin::SignerState::Unsupported;
    }
}

auto toQt(const rust::String &value) -> QString {
    return HwiModule::toQt(value);
}
} // namespace

HwiSigner::HwiSigner(HwiModule *module, const HwiDevice &device, QObject *parent)
    : ISigner(parent)
    , m_module(module)
    , m_id(toQt(device.id))
    , m_kind(toQt(device.kind))
    , m_fingerprint(toQt(device.fingerprint))
    , m_version(toQt(device.version))
    , m_detail(toQt(device.detail))
    , m_state(toState(device.state)) {
    // Queued on purpose: a result must not land before the caller holds the ReqId.
    auto deferred = static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection);
    connect(this, &HwiSigner::deferredInfo, this, &HwiSigner::onDeferredInfo, deferred);
    connect(this, &HwiSigner::deferredInitialized, this, &HwiSigner::onDeferredInitialized,
            deferred);
    connect(this, &HwiSigner::deferredError, this, &HwiSigner::onDeferredError, deferred);
    connect(this, &HwiSigner::deferredIsRegistered, this, &HwiSigner::onDeferredIsRegistered,
            deferred);
}

bool HwiSigner::implemented() const {
    return true;
}

QString HwiSigner::id() const {
    return m_id;
}

QString HwiSigner::fingerprint() const {
    return m_fingerprint;
}

QString HwiSigner::walletName() const {
    // bwk derives the on-device wallet name per descriptor, so there is no single one.
    return QString();
}

QString HwiSigner::brand() const {
    // The device kind the service reports (ledger, coldcard, bitbox02, jade).
    return m_kind;
}

// bwk-hwi reports neither a model nor a serial today (only BitBox carries one, inside
// its id), so these stay empty until the upstream ask lands.
QString HwiSigner::model() const {
    return QString();
}

QString HwiSigner::serial() const {
    return QString();
}

plugin::SignerState HwiSigner::state() const {
    return m_state;
}

QString HwiSigner::stateDetail() const {
    return m_detail;
}

QString HwiSigner::kind() const {
    return m_kind;
}

QString HwiSigner::version() const {
    return m_version;
}

bool HwiSigner::ready() const {
    return m_state == plugin::SignerState::Ready;
}

auto HwiSigner::provesRegistrationByHmac(const QString &kind) -> bool {
    // bwk_hwi::DeviceKind's Display of Ledger and LedgerSimulator.
    return kind == QStringLiteral("ledger") || kind == QStringLiteral("ledger-simulator");
}

auto HwiSigner::storedHmac(const QString &descriptor) const -> QString {
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return {};
    }
    return keys->registrationHmac(HwiModule::network(), m_fingerprint, descriptor);
}

void HwiSigner::storeHmac(const QString &descriptor, const QString &hmac) const {
    auto *keys = Host::get()->keys();
    // The device did register: a proof not stored only costs a registration later.
    if (keys == nullptr ||
        !keys->storeRegistrationHmac(HwiModule::network(), m_fingerprint, descriptor, hmac)) {
        qWarning() << "hwi: registration proof of" << m_fingerprint << "not stored";
        return;
    }
    qDebug() << "hwi: registration proof of" << m_fingerprint << "stored";
}

ReqId HwiSigner::init() {
    ReqId reqId = m_module->nextReqId();
    if (ready()) {
        emit deferredInitialized(reqId);
    } else {
        emit deferredError(reqId, m_detail);
    }
    return reqId;
}

ReqId HwiSigner::info() {
    // The service caches kind, fingerprint and version, so this needs no device round
    // trip; it is still a request, answered on the next event loop turn.
    ReqId reqId = m_module->nextReqId();
    emit deferredInfo(reqId);
    return reqId;
}

ReqId HwiSigner::getXpub(const QString &path) {
    return dispatch(Request::Xpub, path);
}

ReqId HwiSigner::isDescriptorRegistered(const QString &descriptor) {
    // A Ledger keeps no wallet list to ask: its stored proof is the registration. The
    // proof is per seed, so a Coldcard on the same seed is still asked.
    if (ready() && provesRegistrationByHmac(m_kind) && !storedHmac(descriptor).isEmpty()) {
        ReqId reqId = m_module->nextReqId();
        emit deferredIsRegistered(reqId, true);
        return reqId;
    }
    return dispatch(Request::IsRegistered, descriptor);
}

ReqId HwiSigner::registerDescriptor(const QString &descriptor) {
    ReqId reqId = dispatch(Request::Register, descriptor);
    m_register_descriptors.insert(reqId, descriptor);
    return reqId;
}

ReqId HwiSigner::signWithDescriptor(const QString &psbt_base64, const QString &descriptor) {
    return dispatch(Request::Sign, psbt_base64, descriptor);
}

ReqId HwiSigner::displayAddress(const QString &descriptor, const QString &path,
                                const QString &address) {
    ReqId reqId = dispatch(Request::DisplayAddress, descriptor, path);
    m_display_addresses.insert(reqId, address);
    return reqId;
}

ReqId HwiSigner::raw(const QByteArray &request) {
    Q_UNUSED(request);
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, QStringLiteral("hwi: no raw requests"));
    return reqId;
}

ReqId HwiSigner::dispatch(Request request, const QString &first, const QString &second) {
    ReqId reqId = m_module->nextReqId();
    if (!ready()) {
        emit deferredError(reqId, m_detail);
        return reqId;
    }

    bool sent = false;
    switch (request) {
    case Request::Xpub:
        sent = m_module->sendXpub(reqId, m_id, first);
        break;
    case Request::IsRegistered:
        sent = m_module->sendIsRegistered(reqId, m_id, first);
        break;
    case Request::Register:
        sent = m_module->sendRegister(reqId, m_id, first);
        break;
    case Request::Sign:
        sent = m_module->sendSign(reqId, m_id, first, second, storedHmac(second));
        break;
    case Request::DisplayAddress:
        sent = m_module->sendDisplayAddress(reqId, m_id, first, second, storedHmac(first));
        break;
    }

    if (!sent) {
        emit deferredError(reqId, QStringLiteral("hwi: the device did not accept the request"));
        return reqId;
    }
    m_module->trackRequest(reqId, this);
    return reqId;
}

void HwiSigner::deliver(const HwiEvent &event) {
    auto reqId = static_cast<ReqId>(event.req_id);
    switch (event.flag) {
    case HwiEventFlag::Xpub:
        emit xpub(reqId, toQt(event.payload));
        break;
    case HwiEventFlag::WalletRegistered: {
        QString descriptor = m_register_descriptors.take(reqId);
        if (!event.hmac.empty()) {
            storeHmac(descriptor, toQt(event.hmac));
        }
        emit descriptorRegistered(reqId, event.ok);
        break;
    }
    case HwiEventFlag::WalletIsRegistered:
        emit descriptorIsRegistered(reqId, event.ok);
        break;
    case HwiEventFlag::TransactionSigned:
        emit signed_(reqId, toQt(event.payload));
        break;
    case HwiEventFlag::AddressDisplayed:
        emit addressDisplayed(reqId, m_display_addresses.take(reqId));
        break;
    case HwiEventFlag::Error:
        m_display_addresses.remove(reqId);
        m_register_descriptors.remove(reqId);
        emit error(reqId, toQt(event.error),
                   event.error_kind == HwiErrorKind::MusigSession ? plugin::SignError::MusigSession
                                                                  : plugin::SignError::Other);
        break;
    default:
        break;
    }
}

bool HwiSigner::matches(const HwiDevice &device) const {
    return m_kind == toQt(device.kind) && m_fingerprint == toQt(device.fingerprint) &&
           m_version == toQt(device.version) && m_detail == toQt(device.detail) &&
           m_state == toState(device.state);
}

void HwiSigner::onDeferredInfo(ReqId req_id) {
    QMap<QString, QString> details;
    details.insert(QStringLiteral("kind"), m_kind);
    details.insert(QStringLiteral("fingerprint"), m_fingerprint);
    details.insert(QStringLiteral("version"), m_version);
    emit ISigner::info(req_id, details);
}

void HwiSigner::onDeferredInitialized(ReqId req_id) {
    emit initialized(req_id);
}

void HwiSigner::onDeferredError(ReqId req_id, QString message) {
    // Covers a displayAddress() or registerDescriptor() refused before dispatch, so
    // the maps cannot grow there either; a no-op for every other request kind.
    m_display_addresses.remove(req_id);
    m_register_descriptors.remove(req_id);
    emit error(req_id, std::move(message));
}

void HwiSigner::onDeferredIsRegistered(ReqId req_id, bool registered) {
    emit descriptorIsRegistered(req_id, registered);
}
