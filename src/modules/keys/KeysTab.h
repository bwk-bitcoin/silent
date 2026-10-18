#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <Qontrol>
#include <interfaces/types.h>
#include <silent.h>

#include "KeysModule.h"

class ISigner;

namespace catalog {
class ComboBox;
class EmptyState;
class Notice;
class ScrollArea;
class Section;
class SelectRow;
} // namespace catalog

namespace keys {

// The Keys tab: the devices on the bus and the keys of the network, each in its own
// foldable section that scrolls on its own. Polling runs only while the tab is
// visible, so an idle USB bus costs nothing when the user is elsewhere.
// One device being turned into a key: its name and alias, the paths it gives xpubs at,
// the xpubs it gave so far and the request it is answering.
struct KeyCreation {
    QString name;
    QString alias;
    QStringList paths;
    QList<PathXpub> xpubs;
    QPointer<ISigner> signer;
    ReqId req = 0;
};

class KeysTab final : public qontrol::Screen {
    Q_OBJECT

public:
    explicit KeysTab(KeysModule *module);
    ~KeysTab() override = default;

public slots:
    void onNetworkChanged();
    void onDevicesChanged();
    void onKeysChanged();
    void onSectionToggled();
    void onInstanceRemoved(const QString &id);
    void onDeviceClicked();
    void onKeyClicked();
    void openAddKey();
    void onRegisterConfirmed(const QString &alias);
    void onRegisterXpub(ReqId req_id, const QString &xpub);
    void onRegisterError(std::optional<ReqId> req_id, const QString &message);
    void dismissError();

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    void rebuildDevices();
    void rebuildKeys();
    [[nodiscard]] auto deviceRow(const plugin::SignerEntry &entry) const -> catalog::SelectRow *;
    [[nodiscard]] auto keyRow(const KeyInfo &info) const -> catalog::SelectRow *;
    // The alias of the key `fingerprint` is registered as, empty when none is.
    [[nodiscard]] auto keyAlias(const QString &fingerprint) const -> QString;
    // Asks the device of `creation` for the first path it has not given an xpub for yet.
    static void requestRegisterXpub(KeyCreation &creation);
    // The device `instance_id` is no longer being turned into a key.
    void finishRegistering(const QString &instance_id);
    // The creation the device `signer` answers `req_id` for, its instance id; empty when
    // none.
    [[nodiscard]] auto creationFor(const QObject *signer, ReqId req_id) const -> QString;
    // Whether a device holding `fingerprint` is being turned into a key.
    [[nodiscard]] auto creating(const QString &fingerprint) const -> bool;
    void showRegisterError(const QString &device, const QString &message);

private:
    KeysModule *m_module = nullptr;
    catalog::ComboBox *m_network_selector = nullptr;
    catalog::Notice *m_error = nullptr;
    qontrol::Column *m_sections = nullptr;
    catalog::Section *m_devices = nullptr;
    catalog::ScrollArea *m_device_scroll = nullptr;
    qontrol::Column *m_device_column = nullptr;
    catalog::EmptyState *m_devices_empty = nullptr;
    catalog::Section *m_keys = nullptr;
    catalog::ScrollArea *m_key_scroll = nullptr;
    qontrol::Column *m_key_column = nullptr;
    QWidget *m_main_widget = nullptr;
    // The device the register modal was opened for.
    QString m_register_instance;
    // The devices being turned into keys, by device instance id: each runs on its own.
    QHash<QString, KeyCreation> m_creations;
};

} // namespace keys
