#pragma once

#include "state/StateSetup.h"

#include <cstdint>
#include <memory>

namespace catalog {
class Button;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
} // namespace catalog

namespace qontrol {
class Column;
}

namespace state {

class UnlockDevices;

class DeviceFlow final : public StateSetupFlow {
    Q_OBJECT

public:
    explicit DeviceFlow(Network network);
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onDevicesChanged();
    void onDeviceRowClicked();
    void onXpubFetched(quint64 req_id, const QString &xpub);
    void onXpubFailed(quint64 req_id, const QString &error);
    void onDeviceCreated();

protected:
    enum class View : std::uint8_t { Device, Busy };

    void init();
    void doConnect();
    void view();

    // Device view
    void initDevice();
    void connectDevice();
    [[nodiscard]] auto viewDevice() -> QWidget *;
    void showDevice();
    void hideDevice();
    void rebuildDevices();
    void updateDeviceActions();
    void fetchXpub();

    // Busy view
    void initBusy();
    [[nodiscard]] auto viewBusy() -> catalog::ModalStatus *;
    void showBusy(const QString &path);
    void hideBusy();

    // Shared
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    View m_view = View::Device;
    Network m_network = Network::Bitcoin;
    QWidget *m_device = nullptr;
    catalog::Label *m_device_text = nullptr;
    qontrol::Column *m_device_rows = nullptr;
    catalog::ModalStatus *m_no_device = nullptr;
    catalog::Notice *m_device_error = nullptr;
    catalog::ModalActions *m_device_actions = nullptr;
    catalog::Button *m_device_continue = nullptr;
    UnlockDevices *m_devices = nullptr;
    QString m_selected_id;
    QString m_selected_name;
    quint32 m_index = 0;
    quint64 m_xpub_req = 0;
    catalog::ModalStatus *m_busy = nullptr;
    std::shared_ptr<QString> m_result;
};

} // namespace state
