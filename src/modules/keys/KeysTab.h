#pragma once

#include <Qontrol>

namespace catalog {
class Button;
class ComboBox;
class Label;
} // namespace catalog

namespace keys {

class KeysModule;

// The Keys tab. Two lists: the devices currently on the bus, and the keys the user
// registered. Polling runs only while the tab is visible, so an idle USB bus costs
// nothing when the user is elsewhere.
class KeysTab final : public qontrol::Screen {
    Q_OBJECT

public:
    explicit KeysTab(KeysModule *module);
    ~KeysTab() override = default;

public slots:
    void onNetworkChanged();
    void onDevicesChanged();
    void onKeysChanged();
    void onRegisterDeviceClicked();
    void onDeviceClicked();
    void onKeyClicked();

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    void rebuildDevices();
    void rebuildKeys();
    void showError(const QString &message);

private:
    KeysModule *m_module = nullptr;
    catalog::ComboBox *m_network_selector = nullptr;
    catalog::Label *m_devices_empty = nullptr;
    catalog::Label *m_error_label = nullptr;
    qontrol::Column *m_device_column = nullptr;
    catalog::Label *m_keys_empty = nullptr;
    qontrol::Column *m_key_column = nullptr;
    catalog::Button *m_add_key_btn = nullptr;
    QWidget *m_main_widget = nullptr;
};

} // namespace keys
