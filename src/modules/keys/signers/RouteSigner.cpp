#include "RouteSigner.h"

#include "modules/keys/KeysModule.h"
#include "modules/keys/RouteDisplay.h"

#include "i18n/Tr.h"

#include <QMap>

#include <utility>

#include <common.h>
#include <silent.h>

namespace keys {

RouteSigner::RouteSigner(KeysModule *module, const KeyInfo &key, KeyRouteKind route,
                         QObject *parent)
    : ISigner(parent),
      m_module(module),
      m_fingerprint(KeysModule::toQt(key.fingerprint)),
      m_alias(KeysModule::toQt(key.alias)),
      m_route(route) {
    // Queued on purpose: a result must not land before the caller holds the ReqId.
    auto deferred = static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection);
    connect(this, &RouteSigner::deferredInfo, this, &RouteSigner::onDeferredInfo, deferred);
    connect(this, &RouteSigner::deferredInitialized, this, &RouteSigner::onDeferredInitialized,
            deferred);
    connect(this, &RouteSigner::deferredXpub, this, &RouteSigner::onDeferredXpub, deferred);
    connect(this, &RouteSigner::deferredRegistered, this, &RouteSigner::onDeferredRegistered,
            deferred);
    connect(this, &RouteSigner::deferredIsRegistered, this, &RouteSigner::onDeferredIsRegistered,
            deferred);
    connect(this, &RouteSigner::deferredError, this, &RouteSigner::onDeferredError, deferred);
    connect(m_module, &KeysModule::hotSigned, this, &RouteSigner::onHotSigned, qontrol::UNIQUE);
}

bool RouteSigner::implemented() const {
    return true;
}

QString RouteSigner::id() const {
    return m_alias.isEmpty() ? m_fingerprint : m_alias;
}

QString RouteSigner::fingerprint() const {
    return m_fingerprint;
}

QString RouteSigner::walletName() const {
    // Not a device: there is no on-device wallet name to report.
    return QString();
}

// KeyInfo carries no brand/model/serial: those live only on KeyDevice records for
// devices seen over USB. A route signer never wraps a physical device, so these
// stay empty for every route it can serve.
QString RouteSigner::brand() const {
    return QString();
}

QString RouteSigner::model() const {
    return QString();
}

QString RouteSigner::serial() const {
    return QString();
}

plugin::SignerState RouteSigner::state() const {
    // The module only ever builds a RouteSigner for a route it already found usable.
    return plugin::SignerState::Ready;
}

QString RouteSigner::stateDetail() const {
    return QString();
}

auto RouteSigner::route() const -> KeyRouteKind {
    return m_route;
}

ReqId RouteSigner::init() {
    ReqId reqId = m_module->nextReqId();
    emit deferredInitialized(reqId);
    return reqId;
}

ReqId RouteSigner::info() {
    // The record is cached on the C++ side already, so this needs no device round
    // trip; it is still a request, answered on the next event loop turn.
    ReqId reqId = m_module->nextReqId();
    emit deferredInfo(reqId);
    return reqId;
}

ReqId RouteSigner::getXpub(const QString &path) {
    ReqId reqId = m_module->nextReqId();
    if (m_module->rust() == nullptr) {
        emit deferredError(reqId, TR("keys-module-not-ready"));
        return reqId;
    }
    for (const auto &xpub :
         m_module->rust()->key_xpubs(rust::String(m_fingerprint.toStdString()))) {
        if (KeysModule::toQt(xpub.path) == path) {
            emit deferredXpub(reqId, KeysModule::toQt(xpub.xpub));
            return reqId;
        }
    }
    emit deferredError(reqId, TR("keys-xpub-not-cached"));
    return reqId;
}

ReqId RouteSigner::isDescriptorRegistered(const QString &descriptor) {
    ReqId reqId = m_module->nextReqId();
    bool registered = false;
    if (m_module->rust() != nullptr) {
        for (const auto &entry :
             m_module->rust()->key_descriptors(rust::String(m_fingerprint.toStdString()))) {
            if (KeysModule::toQt(entry.descriptor) == descriptor) {
                registered = entry.registered;
                break;
            }
        }
    }
    emit deferredIsRegistered(reqId, registered);
    return reqId;
}

ReqId RouteSigner::registerDescriptor(const QString &descriptor) {
    ReqId reqId = m_module->nextReqId();
    if (m_module->rust() == nullptr) {
        emit deferredError(reqId, TR("keys-module-not-ready"));
        return reqId;
    }
    auto result = m_module->rust()->register_descriptor(rust::String(m_fingerprint.toStdString()),
                                                        rust::String(descriptor.toStdString()));
    if (!result.is_ok) {
        emit deferredError(reqId, KeysModule::toQt(result.error));
        return reqId;
    }
    emit deferredRegistered(reqId, true);
    return reqId;
}

ReqId RouteSigner::signWithDescriptor(const QString &psbt_base64, const QString &descriptor) {
    ReqId reqId = m_module->nextReqId();
    switch (m_route) {
    case KeyRouteKind::Hot:
        signHot(reqId, psbt_base64, descriptor);
        break;
    case KeyRouteKind::Qr:
    case KeyRouteKind::SdCard:
        // The module owns the modal flow for these; it answers via deliverSigned()
        // or deliverError() once the flow ends.
        emit routeSignRequested(reqId, m_fingerprint, psbt_base64, descriptor);
        break;
    case KeyRouteKind::Usb:
    case KeyRouteKind::Unknown:
    default:
        // A connected USB route is served by the device's own ISigner, never by a
        // RouteSigner: this should never be reached.
        emit deferredError(reqId, TR("keys-route-unsupported"));
        break;
    }
    return reqId;
}

ReqId RouteSigner::displayAddress(const QString &descriptor, const QString &path,
                                  const QString &address) {
    ReqId reqId = m_module->nextReqId();
    switch (m_route) {
    case KeyRouteKind::Hot:
        emit deferredError(reqId, TR("keys-hot-no-display"));
        break;
    case KeyRouteKind::Qr:
        m_display_addresses.insert(reqId, address);
        emit routeDisplayRequested(reqId, m_fingerprint, descriptor, path, address);
        break;
    case KeyRouteKind::Usb:
    case KeyRouteKind::SdCard:
    case KeyRouteKind::Unknown:
    default:
        emit deferredError(reqId, TR("keys-route-unsupported"));
        break;
    }
    return reqId;
}

ReqId RouteSigner::raw([[maybe_unused]] const QByteArray &request) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-no-raw-requests"));
    return reqId;
}

void RouteSigner::signHot(ReqId req_id, const QString &psbt_v2_hex,
                          [[maybe_unused]] const QString &descriptor) {
    if (m_module->rust() == nullptr) {
        emit deferredError(req_id, TR("keys-module-not-ready"));
        return;
    }
    // Tracked until onHotSigned()/onPasswordResolved() or an early error path
    // below clears it, so a route-change rebuild never deletes this instance while
    // the module's password prompt is open or the background thread still holds
    // `this`.
    m_module->trackPendingRequest(req_id, this);
    m_pending_signs.insert(req_id, psbt_v2_hex);
    m_module->promptPassword(this, req_id);
}

void RouteSigner::onPasswordResolved(ReqId req_id, QString password) {
    if (!m_pending_signs.contains(req_id)) {
        // Not a request this instance is waiting on.
        return;
    }

    if (password.isEmpty()) {
        // The prompt refuses to confirm an empty password, so this also covers the
        // user cancelling the modal.
        emit deferredError(req_id, TR("keys-signing-cancelled"));
        m_pending_signs.remove(req_id);
        m_module->untrackPendingRequest(req_id);
        return;
    }

    // The module owns the worker thread; it answers through hotSigned(), matched
    // by req_id in onHotSigned().
    m_module->signHot(req_id, m_fingerprint, password, m_pending_signs.value(req_id));

    password.fill(QChar());
    password.clear();
}

void RouteSigner::onHotSigned(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error) {
    if (!m_pending_signs.remove(req_id)) {
        // Not a request this instance is waiting on.
        return;
    }
    m_module->untrackPendingRequest(req_id);
    if (is_ok) {
        emit signed_(req_id, std::move(psbt_v2_hex));
        return;
    }
    emit ISigner::error(req_id, std::move(error));
}

void RouteSigner::deliverSigned(ReqId req_id, const QString &psbt_hex) {
    emit signed_(req_id, psbt_hex);
}

void RouteSigner::deliverDisplayed(ReqId req_id, bool displayed) {
    QString address = m_display_addresses.take(req_id);
    if (displayed) {
        emit addressDisplayed(req_id, address);
        return;
    }
    emit ISigner::error(req_id, TR("keys-address-display-failed"));
}

void RouteSigner::deliverError(ReqId req_id, const QString &message) {
    m_display_addresses.remove(req_id);
    emit ISigner::error(req_id, message);
}

void RouteSigner::onDeferredInfo(ReqId req_id) {
    QMap<QString, QString> details;
    details.insert(QStringLiteral("alias"), m_alias);
    details.insert(QStringLiteral("fingerprint"), m_fingerprint);
    details.insert(QStringLiteral("route"), routeLabel(m_route));
    details.insert(QStringLiteral("brand"), brand());
    details.insert(QStringLiteral("model"), model());
    details.insert(QStringLiteral("serial"), serial());
    details.insert(QStringLiteral("descriptor_count"),
                   QString::number(m_module->countRegisteredDescriptors(m_fingerprint)));
    emit ISigner::info(req_id, details);
}

void RouteSigner::onDeferredInitialized(ReqId req_id) {
    emit initialized(req_id);
}

void RouteSigner::onDeferredXpub(ReqId req_id, QString xpub) {
    emit ISigner::xpub(req_id, std::move(xpub));
}

void RouteSigner::onDeferredRegistered(ReqId req_id, bool registered) {
    emit descriptorRegistered(req_id, registered);
}

void RouteSigner::onDeferredIsRegistered(ReqId req_id, bool registered) {
    emit descriptorIsRegistered(req_id, registered);
}

void RouteSigner::onDeferredError(ReqId req_id, QString message) {
    emit ISigner::error(req_id, std::move(message));
}

} // namespace keys
