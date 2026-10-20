#include "LedgerNonces.h"

#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/KeysModule.h"

#include <QDebug>
#include <QThread>
#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::flow {

LedgerNonces::LedgerNonces(KeysModule *module, ISigner *device, QString instance_id,
                           QString fingerprint, QString descriptor, unsigned int quantity,
                           QObject *owner)
    : m_device(device), m_instance_id(std::move(instance_id)),
      m_fingerprint(std::move(fingerprint)), m_descriptor(std::move(descriptor)),
      m_quantity(quantity), m_work(std::make_shared<Work>()) {
    connect(this, &LedgerNonces::cacheChanged, module, &KeysModule::nonceSessionsChanged,
            qontrol::UNIQUE);
    connect(owner, &QObject::destroyed, this, &LedgerNonces::onCancel, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &LedgerNonces::onInstanceRemoved, qontrol::UNIQUE);
    if (!m_device.isNull()) {
        connect(m_device, &ISigner::signed_, this, &LedgerNonces::onDeviceSigned, qontrol::UNIQUE);
        connect(m_device, &ISigner::error, this, &LedgerNonces::onDeviceError, qontrol::UNIQUE);
        connect(m_device, &QObject::destroyed, this, &LedgerNonces::onDeviceDestroyed,
                qontrol::UNIQUE);
    }
}

void LedgerNonces::start() {
    auto work = m_work;
    m_thread = QThread::create([work, fingerprint = m_fingerprint.toStdString(),
                               descriptor = m_descriptor.toStdString(),
                               quantity = m_quantity]() mutable {
        work->fetch.emplace(::ledger_nonce_fetch(fingerprint, descriptor, quantity));
        work->payload = work->fetch.value()->payload();
        work.reset();
    });
    connect(m_thread, &QThread::finished, this, &LedgerNonces::onWorkFinished, qontrol::UNIQUE);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater, qontrol::UNIQUE);
    m_thread->start();
}

void LedgerNonces::onWorkFinished() {
    m_thread = nullptr;
    emit cacheChanged();
    if (m_cancelled) {
        finish(QString());
        return;
    }
    if (!m_work->payload.is_ok) {
        finish(KeysModule::toQt(m_work->payload.error));
        return;
    }
    QString payload = KeysModule::toQt(m_work->payload.psbt_base64);
    if (payload.isEmpty()) {
        finish(QString());
        return;
    }
    if (m_device.isNull() || m_device->state() != plugin::SignerState::Ready) {
        finish(QStringLiteral("Ledger device disconnected"));
        return;
    }
    m_req = m_device->signWithDescriptor(payload, m_descriptor);
}

void LedgerNonces::onDeviceSigned(ReqId req_id, const QString &psbt_base64) {
    if (m_req == 0 || m_req != req_id) {
        return;
    }
    m_req = 0;
    if (m_cancelled) {
        finish(QString());
        return;
    }
    auto work = m_work;
    m_thread = QThread::create([work, signedPsbt = psbt_base64.toStdString()]() mutable {
        work->payload = work->fetch.value()->accept(signedPsbt);
        work.reset();
    });
    connect(m_thread, &QThread::finished, this, &LedgerNonces::onWorkFinished, qontrol::UNIQUE);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater, qontrol::UNIQUE);
    m_thread->start();
}

void LedgerNonces::onDeviceError(std::optional<ReqId> req_id, const QString &message) {
    if (m_req != 0 && req_id.has_value() && req_id.value() == m_req) {
        m_req = 0;
        finish(message);
    }
}

void LedgerNonces::onInstanceRemoved(const QString &id) {
    if (id != m_instance_id) {
        return;
    }
    m_device = nullptr;
    m_req = 0;
    if (m_thread == nullptr) {
        finish(QStringLiteral("Ledger device disconnected"));
    }
}

void LedgerNonces::onDeviceDestroyed() {
    onInstanceRemoved(m_instance_id);
}

void LedgerNonces::onCancel() {
    m_cancelled = true;
    if (m_thread == nullptr && m_req == 0) {
        finish(QString());
    }
}

void LedgerNonces::finish(const QString &error) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_req = 0;
    disconnect(Host::get()->events(), nullptr, this, nullptr);
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
    // Release the fingerprint lock before the caller tries to allocate a session.
    m_work.reset();
    if (!error.isEmpty()) {
        qWarning() << "Ledger nonce fetch:" << error;
    }
    if (!m_cancelled) {
        emit finished(error);
    }
    deleteLater();
}

} // namespace keys::flow
