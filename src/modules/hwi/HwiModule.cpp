#include "HwiModule.h"

#include "HwiDeviceInstance.h"
#include "HwiServiceInstance.h"
#include "HwiSigner.h"

#include "interfaces/host.h"

#include <QDebug>
#include <QSet>
#include <QThread>

#include <common.h>

namespace {
const QString K_SERVICE_INSTANCE_ID = QStringLiteral("hwi.service");
const QString K_INSTANCE_PREFIX = QStringLiteral("hwi.");
} // namespace

HwiModule::HwiModule(QObject *parent) : IModule(parent) {
    connect(this, &HwiModule::hwiEventReceived, this, &HwiModule::onHwiEventReceived,
            qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &HwiModule::onInstanceRemoved, qontrol::UNIQUE);
    createService();
}

HwiModule::~HwiModule() {
    teardownService();
}

QString HwiModule::toQt(const rust::String &value) {
    return QString::fromStdString(std::string(value));
}

QMap<QString, QString> HwiModule::meta() const {
    return {{QStringLiteral("id"), QStringLiteral("hwi.module")},
            {QStringLiteral("name"), QStringLiteral("Hardware Wallets")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

ReqId HwiModule::list() {
    // Nothing to launch: the service instance auto-starts and devices come and go on
    // their own, so the launcher shows no button for this module.
    ReqId reqId = nextReqId();
    emit IModule::instances(reqId, QList<QPair<QString, QString>>{});
    return reqId;
}

ReqId HwiModule::autoStart() {
    ReqId reqId = nextReqId();
    emit IModule::autoStart(reqId, QStringList{K_SERVICE_INSTANCE_ID});
    return reqId;
}

void HwiModule::startInstance(const QString &id) {
    if (id != K_SERVICE_INSTANCE_ID) {
        emit error(std::nullopt, QStringLiteral("hwi: unknown instance %1").arg(id));
        return;
    }
    if (m_service_instance != nullptr) {
        return;
    }
    m_service_instance = std::make_unique<HwiServiceInstance>(this, id);
    Host::get()->registerInstance(id, m_service_instance.get(), this);
}

void HwiModule::createService() {
    m_service = std::make_optional(::new_hwi_service(::hwi_config_network()));

    auto receiver = m_service.value()->take_receiver();
    m_notif_thread = QThread::create([this, recv = std::move(receiver)]() {
        while (true) {
            auto poll = recv->recv();
            if (!poll->is_some()) {
                break;
            }
            emit hwiEventReceived(poll->get_event());
        }
    });
    connect(m_notif_thread, &QThread::finished, m_notif_thread, &QThread::deleteLater);
    m_notif_thread->start();
}

void HwiModule::teardownService() {
    dropAllDevices();
    if (m_service.has_value()) {
        // Wakes the blocking recv with a sentinel; waiting for the service's listener to
        // drop its sender would hang here for as long as a device transfer takes.
        m_service.value()->shutdown();
    }
    if (m_notif_thread != nullptr) {
        m_notif_thread->wait();
        m_notif_thread = nullptr;
    }
    m_service.reset();
    m_pending.clear();
}

void HwiModule::setSignerPolling(bool enabled) {
    if (m_polling == enabled) {
        return;
    }
    m_polling = enabled;
    if (!m_service.has_value()) {
        return;
    }

    if (enabled) {
        m_service.value()->start();
        return;
    }
    m_service.value()->stop();
    // Nothing is watching the bus any more, so the devices must not stay listed.
    dropAllDevices();
    emit devicesChanged();
}

bool HwiModule::polling() const {
    return m_polling;
}

void HwiModule::requestPolling(bool enabled) {
    Host::get()->requestSignerPolling(this, enabled);
}

QList<HwiDevice> HwiModule::devices() const {
    QList<HwiDevice> devices;
    if (!m_service.has_value()) {
        return devices;
    }
    for (const auto &device : m_service.value()->devices()) {
        devices.append(device);
    }
    return devices;
}

HwiSigner *HwiModule::deviceSigner(const QString &device_id) const {
    auto it = m_devices.find(instanceIdFor(device_id));
    return it == m_devices.end() ? nullptr : it->second->hwiSigner();
}

Network HwiModule::network() {
    return ::hwi_config_network();
}

void HwiModule::setNetwork(Network network) {
    if (::hwi_config_network() == network) {
        return;
    }
    ::hwi_set_config_network(network);
    if (!m_service.has_value()) {
        return;
    }

    // The service rebuilds itself around the same notification channel, so the
    // notification thread stays put. Tearing it down here would block the GUI thread on
    // a listener that can sit inside a device transfer for seconds.
    dropAllDevices();
    m_service.value()->set_network(network);
    emit devicesChanged();
}

ReqId HwiModule::nextReqId() {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}

void HwiModule::trackRequest(ReqId req_id, HwiSigner *signer) {
    m_pending.insert(req_id, signer);
}

bool HwiModule::sendXpub(ReqId req_id, const QString &device_id, const QString &path) {
    if (!m_service.has_value()) {
        return false;
    }
    return m_service.value()->get_xpub(req_id, device_id.toStdString(), path.toStdString());
}

bool HwiModule::sendIsRegistered(ReqId req_id, const QString &device_id,
                                 const QString &descriptor) {
    if (!m_service.has_value()) {
        return false;
    }
    return m_service.value()->is_descriptor_registered(req_id, device_id.toStdString(),
                                                       descriptor.toStdString());
}

bool HwiModule::sendRegister(ReqId req_id, const QString &device_id, const QString &descriptor) {
    if (!m_service.has_value()) {
        return false;
    }
    return m_service.value()->register_descriptor(req_id, device_id.toStdString(),
                                                  descriptor.toStdString());
}

bool HwiModule::sendSign(ReqId req_id, const QString &device_id, const QString &psbt_base64) {
    if (!m_service.has_value()) {
        return false;
    }
    return m_service.value()->sign_psbt(req_id, device_id.toStdString(),
                                        psbt_base64.toStdString());
}

QString HwiModule::instanceIdFor(const QString &device_id) {
    return K_INSTANCE_PREFIX + device_id;
}

void HwiModule::onHwiEventReceived(const HwiEvent &event) {
    if (event.flag == HwiEventFlag::DevicesChanged) {
        syncInstances();
        emit devicesChanged();
        return;
    }

    auto reqId = static_cast<ReqId>(event.req_id);
    auto *signer = m_pending.take(reqId);
    if (signer == nullptr) {
        // A push we never asked for, or an answer to a request whose signer is gone.
        if (event.flag == HwiEventFlag::Error) {
            qWarning() << "hwi:" << toQt(event.error);
        }
        return;
    }
    signer->deliver(event);
}

void HwiModule::syncInstances() {
    auto devices = this->devices();

    QSet<QString> seen;
    for (const auto &device : devices) {
        QString instanceId = instanceIdFor(toQt(device.id));
        seen.insert(instanceId);

        auto it = m_devices.find(instanceId);
        if (it != m_devices.end()) {
            auto *signer = it->second->hwiSigner();
            if (signer != nullptr && signer->matches(device)) {
                continue;
            }
            // The accessors are immutable, so a changed device is a new instance.
            Host::get()->removeInstance(instanceId, this);
        }

        auto instance = std::make_unique<HwiDeviceInstance>(this, device, instanceId);
        Host::get()->registerInstance(instanceId, instance.get(), this);
        m_devices.emplace(instanceId, std::move(instance));
    }

    for (const auto &instanceId : liveInstanceIds()) {
        if (!seen.contains(instanceId)) {
            Host::get()->removeInstance(instanceId, this);
        }
    }
}

QStringList HwiModule::liveInstanceIds() const {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(m_devices.size()));
    for (const auto &entry : m_devices) {
        ids.append(entry.first);
    }
    return ids;
}

void HwiModule::dropAllDevices() {
    for (const auto &instanceId : liveInstanceIds()) {
        Host::get()->removeInstance(instanceId, this);
    }
    m_devices.clear();
}

void HwiModule::onInstanceRemoved(const QString &id) {
    if (id == K_SERVICE_INSTANCE_ID) {
        m_service_instance.reset();
        return;
    }
    // The host has already stopped it and closed any UI it pushed; drop our own copy.
    auto entry = m_devices.find(id);
    if (entry == m_devices.end()) {
        return;
    }
    auto *signer = entry->second->hwiSigner();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        it = it.value() == signer ? m_pending.erase(it) : ++it;
    }
    m_devices.erase(entry);
}
