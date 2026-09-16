#pragma once

#include <memory>
#include <optional>

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <interfaces/keys.h>
#include <interfaces/module.h>
#include <interfaces/types.h>
#include <silent.h>

class HwiModule;
class QThread;

namespace keys {

// The CXX bridge exports its opaque handle as ::KeysModule, so the C++ module lives in
// this namespace and names the Rust side through this alias.
using RustKeys = ::KeysModule;

class KeysModule;

// IModule and IKeys are both QObjects, so one class cannot be both. KeysModule owns
// this bus, registers it with the host, and answers every call through it.
class KeysBus final : public IKeys {
    Q_OBJECT

public:
    explicit KeysBus(KeysModule *module, QObject *parent = nullptr);
    ~KeysBus() override = default;

    [[nodiscard]] QList<KeyEntry> keys(Network network) const override;
    auto newRequest() -> ReqId override;
    void sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors) override;
    void verifyAddress(ReqId req_id, const QString &descriptor, const QString &path,
                       const QString &address) override;
    void spDescriptor(ReqId req_id, const QString &fingerprint) override;
    void registerDescriptor(ReqId req_id, const QString &fingerprint,
                            const QString &descriptor) override;

    // Called by the module, which owns every outstanding request.
    void deliverSigned(ReqId req_id, const QString &psbt_v2_hex);
    void deliverVerified(ReqId req_id, const QString &address);
    void deliverError(std::optional<ReqId> req_id, const QString &message);

private:
    KeysModule *m_module = nullptr;
};

// Runs the keys module's blocking notification receiver on a background thread and
// feeds it the live device list every time the hwi module's device set changes.
class KeysModule final : public IModule, public IPlugin {
    Q_OBJECT
    Q_INTERFACES(IPlugin)

public:
    explicit KeysModule(QObject *parent = nullptr);
    ~KeysModule() override;

    [[nodiscard]] QMap<QString, QString> meta() const override;
    [[nodiscard]] QList<IModule *> modules() const override;
    ReqId list() override;
    ReqId autoStart() override;
    void startInstance(const QString &id) override;

    [[nodiscard]] static Network network();
    void setNetwork(Network network);

    [[nodiscard]] QList<KeyInfo> keys() const;
    [[nodiscard]] bool hasKey(const QString &fingerprint) const;
    // The Rust handle, for the tab and the modals of this module only. Null until the
    // handle is built, so every caller checks.
    [[nodiscard]] RustKeys *rust() const;
    // The hwi module, once one has been seen on the bus. Null before that.
    [[nodiscard]] HwiModule *hwi() const;
    [[nodiscard]] KeysBus *bus() const;
    ReqId nextReqId();

    [[nodiscard]] static auto toQt(const rust::String &value) -> QString;

signals:
    // Emitted from the receiver thread, handled on the GUI thread.
    void keyEventReceived(KeyEvent event);
    // The key records changed; the tab rebuilds on it.
    void keysChanged();
    // The live device set changed and the Rust handle already has it.
    void liveDevicesChanged();

private slots:
    void onKeyEventReceived(const KeyEvent &event);
    void onInstanceRegistered(const QString &id);
    void onInstanceRemoved(const QString &id);
    void onDevicesChanged();

protected:
    void createModule();
    void teardownModule();
    void refreshLiveDevices();
    void bindHwiModule();

private:
    // Box's constness gates the pointee's constness through operator->(), so this is
    // mutable to let the const rust() accessor hand callers a mutable handle.
    mutable std::optional<rust::Box<RustKeys>> m_rust;
    QThread *m_notif_thread = nullptr;
    std::unique_ptr<KeysBus> m_bus;
    HwiModule *m_hwi = nullptr;
    ReqId m_next_req_id = 0;
    // The network the Rust manager was last synced to. Guards setNetwork() against its
    // own state rather than ::hwi_config_network(), which HwiModule::setNetwork also
    // writes and may already reflect the incoming value by the time this runs.
    Network m_synced_network = KeysModule::network();
};

} // namespace keys
