#pragma once

#include <QWidget>
#include <Qontrol>
#include <cstdint>
#include <silent.h>

class AccountController;
enum class Endpoint : std::uint8_t;

namespace catalog {
class StatusBarItem;
}

class StatusBar : public QWidget {
    Q_OBJECT

public:
    explicit StatusBar(AccountController *controller, QWidget *parent = nullptr);

public slots:
    void onUpdateConnectionState(bool connected);
    void onUpdateScanProgress(uint32_t height, uint32_t tip);
    void onUpdateWaitingForBlocks(uint32_t tip_height);
    void onElectrumConnected(const QString &address);
    void onElectrumDisconnected();
    void onReloadUrl();
    void onEndpointStateChanged(Endpoint endpoint);
    void onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                   const EndpointResult &result);

protected:
    void onToggled(bool checked);
    void onElectrumToggled(bool checked);
    void initUI();

private:
    AccountController *m_controller = nullptr;
    catalog::StatusBarItem *m_blindbit_item = nullptr;
    QString m_blindbit_url;
    uint64_t m_blindbit_req = 0;

    catalog::StatusBarItem *m_electrum_item = nullptr;
    QString m_electrum_url;
    uint64_t m_electrum_req = 0;
};
