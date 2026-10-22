#include "FakeUnlockDevices.h"

#include "state/UnlockDevices.h"

#include <QThread>
#include <Qontrol>
#include <common.h>
#include <string>

namespace state {

UnlockDevices::UnlockDevices(Network network, QObject *parent)
    : QObject(parent),
      m_service(::new_hwi_service(network)) {
    // The real service is never started. No discovery or confirmation reaches hardware.
    if (setup_test::listDevice) {
        HwiDevice device;
        device.id = "software-fixture";
        device.kind = "coldcard";
        device.fingerprint = "73c5da0a";
        device.state = HwiDeviceState::Supported;
        m_devices.append(device);
    }
    ++setup_test::discoveries;
    ++setup_test::pending;
    m_thread = QThread::create([] {
        QThread::msleep(100);
        --setup_test::pending;
    });
    connect(m_thread, &QThread::finished, this, &UnlockDevices::onPollDevices,
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater, qontrol::UNIQUE);
    m_thread->start();
    connect(this, &UnlockDevices::hwiEventReceived, this, &UnlockDevices::onHwiEventReceived,
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
}

UnlockDevices::~UnlockDevices() {
    ++setup_test::releases;
}

auto UnlockDevices::devices() const -> QList<HwiDevice> {
    return m_devices;
}

auto UnlockDevices::fetchXpub([[maybe_unused]] const QString &device_id,
                              [[maybe_unused]] const QString &path) -> quint64 {
    ++setup_test::requests;
    return setup_test::refuseRequest ? 0 : ++m_next_req_id;
}

void UnlockDevices::onHwiEventReceived(const HwiEvent &event) {
    ++setup_test::deliveries;
    switch (event.flag) {
    case HwiEventFlag::Xpub:
        emit xpubFetched(event.req_id, QString::fromStdString(std::string(event.payload)));
        break;
    case HwiEventFlag::Error:
        emit xpubFailed(event.req_id, QString::fromStdString(std::string(event.error)));
        break;
    default:
        onPollDevices();
        break;
    }
}

void UnlockDevices::onPollDevices() {
    ++setup_test::deliveries;
    emit devicesChanged();
}

} // namespace state
