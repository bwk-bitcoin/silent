#include "RouteSigner.h"

#include "modules/keys/KeysModule.h"

#include "catalog/display/RouteIcons.h"
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

ReqId RouteSigner::signWithDescriptor([[maybe_unused]] const QString &psbt_base64,
                                      [[maybe_unused]] const QString &descriptor) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-route-unsupported"));
    return reqId;
}

ReqId RouteSigner::displayAddress([[maybe_unused]] const QString &descriptor,
                                  [[maybe_unused]] const QString &path,
                                  [[maybe_unused]] const QString &address) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-route-unsupported"));
    return reqId;
}

ReqId RouteSigner::raw([[maybe_unused]] const QByteArray &request) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-no-raw-requests"));
    return reqId;
}

void RouteSigner::onDeferredInfo(ReqId req_id) {
    QMap<QString, QString> details;
    details.insert(QStringLiteral("alias"), m_alias);
    details.insert(QStringLiteral("fingerprint"), m_fingerprint);
    details.insert(QStringLiteral("route"), catalog::routeLabel(m_route));
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
