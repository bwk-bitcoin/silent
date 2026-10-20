#include "DeviceSign.h"

#include "LedgerNonces.h"

#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/KeysModule.h"

#include <QThread>
#include <Qontrol>

#include <memory>
#include <utility>

#include <common.h>
#include <silent.h>

namespace keys::flow {

auto usbErrorFor(const QString &message) -> UsbError {
    if (message.contains(QStringLiteral("User refused"), Qt::CaseInsensitive)) {
        return UsbError::Rejected;
    }
    if (message.contains(QStringLiteral("Device disconnected"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("Device not found"), Qt::CaseInsensitive)) {
        return UsbError::Disconnected;
    }
    return UsbError::Failed;
}

DeviceSign::DeviceSign(KeysModule *module, ISigner *device, QString instance_id,
                       QString psbt_v2_hex, QString fingerprint, QObject *parent)
    : QObject(parent),
      m_module(module),
      m_device(device),
      m_instance_id(std::move(instance_id)),
      m_psbt_v2_hex(std::move(psbt_v2_hex)),
      m_fingerprint(device != nullptr ? device->fingerprint() : std::move(fingerprint)) {
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &DeviceSign::onInstanceRemoved, qontrol::UNIQUE);
}

DeviceSign::~DeviceSign() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
}

void DeviceSign::start(const QString &descriptor) {
    m_descriptor = descriptor;
    bool ledger = m_device.isNull() ||
                  m_device->brand().startsWith(QStringLiteral("ledger"), Qt::CaseInsensitive);
    if (ledger) {
        prepareLedger();
        return;
    }
    // A Coldcard needs a MuSig2 session open for a wallet with a musig() internal key,
    // even on a single-key leaf: the payload then lists that key's participants.
    bool coldcard = m_device->brand().startsWith(QStringLiteral("coldcard"), Qt::CaseInsensitive);
    auto psbt = m_psbt_v2_hex.toStdString();
    auto wallet = descriptor.toStdString();
    auto payload = ::usb_sign_payload(rust::Str(psbt), rust::Str(wallet), coldcard);
    if (!payload.is_ok) {
        fail(UsbError::Failed, KeysModule::toQt(payload.error));
        return;
    }
    sendPayload(KeysModule::toQt(payload.psbt_base64));
}

void DeviceSign::prepareLedger() {
    m_prepared = std::make_shared<LedgerNonceSignResult>();
    auto prepared = m_prepared;
    auto *thread = QThread::create([prepared, fingerprint = m_fingerprint.toStdString(),
                                    descriptor = m_descriptor.toStdString(),
                                    psbt = m_psbt_v2_hex.toStdString()] {
        *prepared = ::ledger_nonce_sign(fingerprint, descriptor, psbt);
    });
    connect(thread, &QThread::finished, this, &DeviceSign::onPrepared, qontrol::UNIQUE);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void DeviceSign::onPrepared() {
    if (m_module.isNull()) {
        fail(UsbError::Failed, TR("keys-module-not-ready"));
        return;
    }
    if (m_prepared->musig) {
        emit m_module->nonceSessionsChanged();
    }
    if (!m_prepared->is_ok) {
        fail(UsbError::Failed, KeysModule::toQt(m_prepared->error),
             m_prepared->musig_session ? plugin::SignError::MusigSession
                                      : plugin::SignError::Other);
        return;
    }
    if (!m_prepared->psbt_v2_hex.empty()) {
        emit signedPsbt(KeysModule::toQt(m_prepared->psbt_v2_hex));
        finish();
        return;
    }
    if (m_prepared->need_fetch) {
        fetchNonces();
        return;
    }
    m_ledger_musig = m_prepared->musig;
    m_replenish = m_ledger_musig && m_prepared->from_cache;
    sendPayload(KeysModule::toQt(m_prepared->psbt_base64));
}

void DeviceSign::fetchNonces() {
    if (m_module.isNull()) {
        fail(UsbError::Failed, TR("keys-module-not-ready"));
        return;
    }
    if (m_device.isNull() || m_device->state() != plugin::SignerState::Ready) {
        fail(UsbError::Disconnected, TR("keys-nonces-connect"));
        return;
    }
    auto *fetch =
        new LedgerNonces(m_module, m_device, m_instance_id, m_fingerprint, m_descriptor, 1, this,
                          m_psbt_v2_hex);
    connect(fetch, &LedgerNonces::finished, this, &DeviceSign::onNoncesFetched, qontrol::UNIQUE);
    fetch->start();
}

void DeviceSign::onNoncesFetched(const QString &error) {
    if (!error.isEmpty()) {
        fail(usbErrorFor(error), error);
        return;
    }
    prepareLedger();
}

void DeviceSign::sendPayload(const QString &payload) {
    if (m_device.isNull()) {
        fail(UsbError::Disconnected, QString());
        return;
    }
    connect(m_device, &ISigner::signed_, this, &DeviceSign::onDeviceSigned, qontrol::UNIQUE);
    connect(m_device, &ISigner::error, this, &DeviceSign::onDeviceError, qontrol::UNIQUE);
    m_req = m_device->signWithDescriptor(payload, m_descriptor);
}

void DeviceSign::onDeviceSigned(ReqId req_id, const QString &psbt_base64) {
    if (m_req == 0 || req_id != m_req) {
        return;
    }
    m_req = 0;
    m_result = std::make_shared<SignResult>();
    auto result = m_result;
    auto *thread = QThread::create([result, ledger = m_ledger_musig,
                                    fingerprint = m_fingerprint.toStdString(),
                                    descriptor = m_descriptor.toStdString(),
                                    original = m_psbt_v2_hex.toStdString(),
                                    signedPsbt = psbt_base64.toStdString()] {
        *result = ledger ? ::ledger_nonce_merge(fingerprint, descriptor, original, signedPsbt)
                         : ::usb_merge_signed(original, signedPsbt);
    });
    connect(thread, &QThread::finished, this, &DeviceSign::onMerged, qontrol::UNIQUE);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void DeviceSign::onMerged() {
    auto &merged = *m_result;
    if (m_ledger_musig && !m_module.isNull()) {
        emit m_module->nonceSessionsChanged();
    }
    if (!merged.is_ok) {
        fail(UsbError::Failed, KeysModule::toQt(merged.error),
             merged.musig_session ? plugin::SignError::MusigSession : plugin::SignError::Other);
        return;
    }
    if (m_replenish && !m_module.isNull() && !m_device.isNull() &&
        m_device->state() == plugin::SignerState::Ready) {
        // The replacement outlives this request; its failure cannot lose the signature.
        auto *replacement =
            new LedgerNonces(m_module, m_device, m_instance_id, m_fingerprint,
                              m_descriptor, 1, m_module);
        replacement->start();
    }
    emit signedPsbt(KeysModule::toQt(merged.psbt_v2_hex));
    finish();
}

void DeviceSign::onDeviceError(std::optional<ReqId> req_id, const QString &message,
                               plugin::SignError kind) {
    if (m_req == 0 || !req_id.has_value() || req_id.value() != m_req) {
        return;
    }
    m_req = 0;
    if (m_ledger_musig && kind == plugin::SignError::MusigSession) {
        m_device_error = message;
        m_result = std::make_shared<SignResult>();
        auto result = m_result;
        auto *thread = QThread::create([result, fingerprint = m_fingerprint.toStdString(),
                                        descriptor = m_descriptor.toStdString(),
                                        original = m_psbt_v2_hex.toStdString()] {
            *result = ::ledger_nonce_invalidate(fingerprint, descriptor, original);
        });
        connect(thread, &QThread::finished, this, &DeviceSign::onInvalidated, qontrol::UNIQUE);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
        thread->start();
        return;
    }
    fail(usbErrorFor(message), message, kind);
}

void DeviceSign::onInvalidated() {
    if (!m_module.isNull()) {
        emit m_module->nonceSessionsChanged();
    }
    if (!m_result->is_ok) {
        fail(UsbError::Failed, KeysModule::toQt(m_result->error),
             m_result->musig_session ? plugin::SignError::MusigSession : plugin::SignError::Other);
        return;
    }
    fail(UsbError::Failed, m_device_error, plugin::SignError::MusigSession);
}

void DeviceSign::onInstanceRemoved(const QString &id) {
    if (m_req == 0 || id != m_instance_id) {
        return;
    }
    fail(UsbError::Disconnected, QString());
}

void DeviceSign::finish() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
    disconnect(Host::get()->events(), nullptr, this, nullptr);
    m_req = 0;
    deleteLater();
}

void DeviceSign::fail(UsbError error, const QString &message, plugin::SignError kind) {
    emit failed(error, message, kind);
    finish();
}

} // namespace keys::flow
