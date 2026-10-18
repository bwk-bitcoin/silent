#include "DeviceQuery.h"

#include "interfaces/host.h"
#include "interfaces/signer.h"

#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::flow {

DeviceQuery::DeviceQuery(ISigner *device, QString instance_id, QObject *parent)
    : QObject(parent),
      m_device(device),
      m_instance_id(std::move(instance_id)) {
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &DeviceQuery::onInstanceRemoved, qontrol::UNIQUE);
}

DeviceQuery::~DeviceQuery() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
}

void DeviceQuery::fetchXpub(const QString &path) {
    if (m_device.isNull()) {
        fail(UsbError::Disconnected, QString());
        return;
    }
    connect(m_device, &ISigner::xpub, this, &DeviceQuery::onDeviceXpub, qontrol::UNIQUE);
    connect(m_device, &ISigner::error, this, &DeviceQuery::onDeviceError, qontrol::UNIQUE);
    m_req = m_device->getXpub(path);
}

void DeviceQuery::showAddress(const QString &descriptor, const QString &path,
                              const QString &address) {
    if (m_device.isNull()) {
        fail(UsbError::Disconnected, QString());
        return;
    }
    m_showing_address = true;
    connect(m_device, &ISigner::addressDisplayed, this, &DeviceQuery::onDeviceAddressDisplayed,
            qontrol::UNIQUE);
    connect(m_device, &ISigner::error, this, &DeviceQuery::onDeviceError, qontrol::UNIQUE);
    m_req = m_device->displayAddress(descriptor, path, address);
}

void DeviceQuery::onDeviceXpub(ReqId req_id, const QString &xpub) {
    if (m_req == 0 || req_id != m_req) {
        return;
    }
    emit xpubFetched(xpub);
    finish();
}

void DeviceQuery::onDeviceAddressDisplayed(ReqId req_id, [[maybe_unused]] const QString &address) {
    if (m_req == 0 || req_id != m_req) {
        return;
    }
    emit addressShown(true);
    finish();
}

void DeviceQuery::onDeviceError(std::optional<ReqId> req_id, const QString &message,
                                [[maybe_unused]] plugin::SignError kind) {
    if (m_req == 0 || !req_id.has_value() || req_id.value() != m_req) {
        return;
    }
    auto error = usbErrorFor(message);
    // Refusing to confirm an address is the device's answer that it does not match.
    if (m_showing_address && error == UsbError::Rejected) {
        emit addressShown(false);
        finish();
        return;
    }
    fail(error, message);
}

void DeviceQuery::onInstanceRemoved(const QString &id) {
    if (m_req == 0 || id != m_instance_id) {
        return;
    }
    fail(UsbError::Disconnected, QString());
}

void DeviceQuery::finish() {
    if (!m_device.isNull()) {
        disconnect(m_device, nullptr, this, nullptr);
    }
    disconnect(Host::get()->events(), nullptr, this, nullptr);
    m_req = 0;
    deleteLater();
}

void DeviceQuery::fail(UsbError error, const QString &message) {
    emit failed(error, message);
    finish();
}

} // namespace keys::flow
