#pragma once

#include <optional>

#include <QPointer>
#include <QString>

#include <Qontrol>
#include <interfaces/types.h>
#include <silent.h>

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

class KeysModule;

// The Keys tab: the devices on the bus and the keys of the network, each in its own
// foldable section that scrolls on its own. Polling runs only while the tab is
// visible, so an idle USB bus costs nothing when the user is elsewhere.
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
    void finishRegistering();
    void showRegisterError(const QString &message);

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
    // The device the register modal was opened for, then the one registering.
    QString m_register_instance;
    QString m_register_name;
    QString m_register_alias;
    QString m_register_path;
    QPointer<ISigner> m_register_signer;
    ReqId m_register_req = 0;
};

} // namespace keys
