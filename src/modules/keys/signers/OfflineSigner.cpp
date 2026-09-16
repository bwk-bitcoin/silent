#include "OfflineSigner.h"

#include "modules/keys/KeysModule.h"

#include "i18n/Tr.h"

#include <QMap>

#include <utility>

#include <silent.h>

namespace keys {

OfflineSigner::OfflineSigner(KeysModule *module, const KeyInfo &key, QObject *parent)
    : ISigner(parent),
      m_module(module),
      m_fingerprint(KeysModule::toQt(key.fingerprint)),
      m_alias(KeysModule::toQt(key.alias)) {
    // Queued on purpose: a result must not land before the caller holds the ReqId.
    auto deferred = static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection);
    connect(this, &OfflineSigner::deferredInfo, this, &OfflineSigner::onDeferredInfo, deferred);
    connect(this, &OfflineSigner::deferredIsRegistered, this,
            &OfflineSigner::onDeferredIsRegistered, deferred);
    connect(this, &OfflineSigner::deferredXpub, this, &OfflineSigner::onDeferredXpub, deferred);
    connect(this, &OfflineSigner::deferredError, this, &OfflineSigner::onDeferredError, deferred);
}

bool OfflineSigner::implemented() const {
    return true;
}

QString OfflineSigner::id() const {
    return m_alias.isEmpty() ? m_fingerprint : m_alias;
}

QString OfflineSigner::fingerprint() const {
    return m_fingerprint;
}

QString OfflineSigner::walletName() const {
    return QString();
}

// KeyInfo carries no brand/model/serial: those live only on KeyDevice records for
// devices seen over USB, unrelated to why this key has no usable route right now.
QString OfflineSigner::brand() const {
    return QString();
}

QString OfflineSigner::model() const {
    return QString();
}

QString OfflineSigner::serial() const {
    return QString();
}

plugin::SignerState OfflineSigner::state() const {
    return plugin::SignerState::Unsupported;
}

QString OfflineSigner::stateDetail() const {
    return TR("keys-no-usable-route");
}

ReqId OfflineSigner::init() {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-no-usable-route"));
    return reqId;
}

ReqId OfflineSigner::info() {
    // The record is cached on the C++ side already and is available no matter the
    // route, so this needs no device round trip.
    ReqId reqId = m_module->nextReqId();
    emit deferredInfo(reqId);
    return reqId;
}

ReqId OfflineSigner::getXpub(const QString &path) {
    ReqId reqId = m_module->nextReqId();
    if (m_module->rust() != nullptr) {
        for (const auto &xpub :
             m_module->rust()->key_xpubs(rust::String(m_fingerprint.toStdString()))) {
            if (KeysModule::toQt(xpub.path) == path) {
                emit deferredXpub(reqId, KeysModule::toQt(xpub.xpub));
                return reqId;
            }
        }
    }
    emit deferredError(reqId, TR("keys-xpub-not-cached"));
    return reqId;
}

ReqId OfflineSigner::isDescriptorRegistered(const QString &descriptor) {
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

ReqId OfflineSigner::registerDescriptor([[maybe_unused]] const QString &descriptor) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-no-usable-route"));
    return reqId;
}

ReqId OfflineSigner::signWithDescriptor([[maybe_unused]] const QString &psbt_base64,
                                        [[maybe_unused]] const QString &descriptor) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-no-usable-route"));
    return reqId;
}

ReqId OfflineSigner::displayAddress([[maybe_unused]] const QString &descriptor,
                                    [[maybe_unused]] const QString &path,
                                    [[maybe_unused]] const QString &address) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-no-usable-route"));
    return reqId;
}

ReqId OfflineSigner::raw([[maybe_unused]] const QByteArray &request) {
    ReqId reqId = m_module->nextReqId();
    emit deferredError(reqId, TR("keys-no-usable-route"));
    return reqId;
}

void OfflineSigner::onDeferredInfo(ReqId req_id) {
    QMap<QString, QString> details;
    details.insert(QStringLiteral("alias"), m_alias);
    details.insert(QStringLiteral("fingerprint"), m_fingerprint);
    details.insert(QStringLiteral("brand"), brand());
    details.insert(QStringLiteral("model"), model());
    details.insert(QStringLiteral("serial"), serial());
    details.insert(QStringLiteral("descriptor_count"),
                   QString::number(m_module->countRegisteredDescriptors(m_fingerprint)));
    emit ISigner::info(req_id, details);
}

void OfflineSigner::onDeferredIsRegistered(ReqId req_id, bool registered) {
    emit descriptorIsRegistered(req_id, registered);
}

void OfflineSigner::onDeferredXpub(ReqId req_id, QString xpub) {
    emit ISigner::xpub(req_id, std::move(xpub));
}

void OfflineSigner::onDeferredError(ReqId req_id, QString message) {
    emit ISigner::error(req_id, std::move(message));
}

} // namespace keys
