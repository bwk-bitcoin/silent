#include "DeviceSign.h"

#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/KeysModule.h"

#include <Qontrol>

#include <utility>

#include <common.h>

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

DeviceSign::DeviceSign(ISigner *device, QString instance_id, QString psbt_v2_hex, QObject *parent)
    : QObject(parent),
      m_device(device),
      m_instance_id(std::move(instance_id)),
      m_psbt_v2_hex(std::move(psbt_v2_hex)) {
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &DeviceSign::onInstanceRemoved, qontrol::UNIQUE);
}

DeviceSign::~DeviceSign() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
}

void DeviceSign::start(const QString &descriptor) {
    if (m_device.isNull()) {
        fail(UsbError::Disconnected, QString());
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
    connect(m_device, &ISigner::signed_, this, &DeviceSign::onDeviceSigned, qontrol::UNIQUE);
    connect(m_device, &ISigner::error, this, &DeviceSign::onDeviceError, qontrol::UNIQUE);
    m_req = m_device->signWithDescriptor(KeysModule::toQt(payload.psbt_base64), descriptor);
}

void DeviceSign::onDeviceSigned(ReqId req_id, const QString &psbt_base64) {
    if (m_req == 0 || req_id != m_req) {
        return;
    }
    auto merged = ::usb_merge_signed(rust::String(m_psbt_v2_hex.toStdString()),
                                     rust::String(psbt_base64.toStdString()));
    if (!merged.is_ok) {
        fail(UsbError::Failed, KeysModule::toQt(merged.error));
        return;
    }
    emit signedPsbt(KeysModule::toQt(merged.psbt_v2_hex));
    finish();
}

void DeviceSign::onDeviceError(std::optional<ReqId> req_id, const QString &message,
                               plugin::SignError kind) {
    if (m_req == 0 || !req_id.has_value() || req_id.value() != m_req) {
        return;
    }
    fail(usbErrorFor(message), message, kind);
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
