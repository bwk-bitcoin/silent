#pragma once

#include <QWidget>
#include <Qontrol>
#include <silent.h>

class AccountController;

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
    void onUpdateScanError(rust::String error, BackendErrorKind kind);
    void onElectrumConnected(const QString &address);
    void onElectrumDisconnected();
    void onReloadUrl();

protected:
    void onToggled(bool checked);
    void onElectrumToggled(bool checked);
    void loadBlindbitUrl();
    void loadElectrumUrl();
    void initUI();

private:
    AccountController *m_controller = nullptr;
    catalog::StatusBarItem *m_blindbit_item = nullptr;
    QString m_blindbit_url;
    bool m_connected = false;

    catalog::StatusBarItem *m_electrum_item = nullptr;
    QString m_electrum_url;
    bool m_electrum_connected = false;
};
