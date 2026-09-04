#pragma once

#include <optional>

#include <QList>
#include <QString>

#include <Qontrol>
#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class ComboBox;
class Label;
class Input;
} // namespace catalog

class HwiModule;
class HwiSigner;

// The HWI settings section: a network selector and the device list, with per-device
// info and xpub actions. Polling runs only while this page is visible, so an unplugged
// USB bus costs nothing when the section is closed.
class HwiSettings final : public qontrol::Screen {
    Q_OBJECT

public:
    explicit HwiSettings(HwiModule *module);
    ~HwiSettings() override = default;

public slots:
    void onNetworkChanged();
    void onInfoClicked();
    void onXpubClicked();
    void onDevicesChanged();
    void onSignerInfo(ReqId req_id, const QMap<QString, QString> &info);
    void onSignerXpub(ReqId req_id, const QString &xpub);
    void onSignerError(std::optional<ReqId> req_id, const QString &message);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    // One device as rendered: the widgets that show its answers, and the requests it
    // is waiting on.
    struct DeviceRow {
        QString device_id;
        HwiSigner *signer = nullptr;
        catalog::Input *path = nullptr;
        catalog::Label *result = nullptr;
        ReqId pending_info = 0;
        ReqId pending_xpub = 0;
    };

    void rebuildDevices();
    [[nodiscard]] auto rowFor(const HwiSigner *signer) -> DeviceRow *;
    [[nodiscard]] auto rowForSender() -> DeviceRow *;

private:
    HwiModule *m_module = nullptr;
    catalog::ComboBox *m_network_selector = nullptr;
    catalog::Label *m_empty_label = nullptr;
    qontrol::Column *m_device_column = nullptr;
    QWidget *m_main_widget = nullptr;
    QList<DeviceRow> m_rows;
};
