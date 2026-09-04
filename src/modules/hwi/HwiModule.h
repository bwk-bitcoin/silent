#pragma once

#include <map>
#include <memory>
#include <optional>

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <interfaces/module.h>
#include <interfaces/types.h>
#include <silent.h>

class HwiDeviceInstance;
class HwiServiceInstance;
class HwiSigner;
class QThread;

// Runs bwk-hwi's device service and keeps one instance per device.
//
// The service owns discovery: it finds devices and maintains the list, and says so with
// a DevicesChanged notification. This module reacts to that by making its instance set
// match, and forwards requests, routing each answer back by the request id it carries.
class HwiModule final : public IModule {
    Q_OBJECT

public:
    explicit HwiModule(QObject *parent = nullptr);
    ~HwiModule() override;

    [[nodiscard]] QMap<QString, QString> meta() const override;
    ReqId list() override;
    ReqId autoStart() override;
    void startInstance(const QString &id) override;
    void setSignerPolling(bool enabled) override;

    // For the settings page.
    [[nodiscard]] QList<HwiDevice> devices() const;
    // The signer for a device id, or null when no instance holds it.
    [[nodiscard]] HwiSigner *deviceSigner(const QString &device_id) const;
    [[nodiscard]] static Network network();
    void setNetwork(Network network);
    void requestPolling(bool enabled);
    [[nodiscard]] bool polling() const;

    // For HwiSigner: dispatch to the device, false when it could not be sent.
    [[nodiscard]] bool sendXpub(ReqId req_id, const QString &device_id, const QString &path);
    [[nodiscard]] bool sendIsRegistered(ReqId req_id, const QString &device_id,
                                        const QString &descriptor);
    [[nodiscard]] bool sendRegister(ReqId req_id, const QString &device_id,
                                    const QString &descriptor);
    [[nodiscard]] bool sendSign(ReqId req_id, const QString &device_id, const QString &psbt_base64);
    ReqId nextReqId();
    void trackRequest(ReqId req_id, HwiSigner *signer);

    [[nodiscard]] static QString toQt(const rust::String &value);

signals:
    void instances(ReqId req_id, QList<QPair<QString, QString>> instances);
    // Emitted from the notification thread, handled on the GUI thread.
    void hwiEventReceived(HwiEvent event);
    // The device list changed; the settings page rebuilds on it.
    void devicesChanged();

private slots:
    void onHwiEventReceived(const HwiEvent &event);
    void onInstanceRemoved(const QString &id);

protected:
    void createService();
    void teardownService();
    void syncInstances();
    void startService();
    void stopService();
    void dropAllDevices();
    [[nodiscard]] QStringList liveInstanceIds() const;
    [[nodiscard]] static QString instanceIdFor(const QString &device_id);

private:
    std::optional<rust::Box<HwiService>> m_service;
    QThread *m_notif_thread = nullptr;
    std::unique_ptr<HwiServiceInstance> m_service_instance;
    std::map<QString, std::unique_ptr<HwiDeviceInstance>> m_devices;
    QHash<ReqId, HwiSigner *> m_pending;
    ReqId m_next_req_id = 0;
    bool m_polling = false;
};
