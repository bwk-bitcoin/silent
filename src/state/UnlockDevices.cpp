#include "UnlockDevices.h"

#include <QThread>
#include <QTimer>
#include <Qontrol>
#include <common.h>
#include <string>
#include <utility>

namespace state {

namespace {
constexpr int POLL_MS = 500;

auto toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

auto sameDevices(const QList<HwiDevice> &a, const QList<HwiDevice> &b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].fingerprint != b[i].fingerprint ||
            a[i].state != b[i].state || a[i].detail != b[i].detail) {
            return false;
        }
    }
    return true;
}
} // namespace

UnlockDevices::UnlockDevices(Network network, QObject *parent)
    : QObject(parent),
      m_service(::new_hwi_service(network)) {
    connect(this, &UnlockDevices::hwiEventReceived, this, &UnlockDevices::onHwiEventReceived,
            qontrol::UNIQUE);
    auto receiver = m_service->take_receiver();
    m_thread = QThread::create([this, recv = std::move(receiver)]() {
        while (true) {
            auto poll = recv->recv();
            if (!poll->is_some()) {
                break;
            }
            emit hwiEventReceived(poll->get_event());
        }
    });
    m_thread->start();
    m_service->start();

    m_poll = new QTimer(this);
    m_poll->setInterval(POLL_MS);
    connect(m_poll, &QTimer::timeout, this, &UnlockDevices::onPollDevices, qontrol::UNIQUE);
    m_poll->start();
    onPollDevices();
}

UnlockDevices::~UnlockDevices() {
    m_poll->stop();
    // Wakes the blocking recv with a sentinel.
    m_service->shutdown();
    m_thread->wait();
    delete m_thread;
}

auto UnlockDevices::devices() const -> QList<HwiDevice> {
    return m_devices;
}

auto UnlockDevices::fetchXpub(const QString &device_id, const QString &path) -> quint64 {
    quint64 reqId = ++m_next_req_id;
    // Shown: a Ledger gives the unlock path, outside the standard ones, only so.
    if (!m_service->get_xpub_shown(reqId, device_id.toStdString(), path.toStdString())) {
        return 0;
    }
    return reqId;
}

void UnlockDevices::onHwiEventReceived(const HwiEvent &event) {
    switch (event.flag) {
    case HwiEventFlag::DevicesChanged:
        onPollDevices();
        return;
    case HwiEventFlag::Xpub:
        emit xpubFetched(event.req_id, toQt(event.payload));
        return;
    case HwiEventFlag::Error:
        if (event.req_id != 0) {
            emit xpubFailed(event.req_id, toQt(event.error));
        }
        return;
    default:
        return;
    }
}

void UnlockDevices::onPollDevices() {
    QList<HwiDevice> devices;
    for (const auto &device : m_service->devices()) {
        devices.append(device);
    }
    if (sameDevices(devices, m_devices)) {
        return;
    }
    m_devices = devices;
    emit devicesChanged();
}

} // namespace state
