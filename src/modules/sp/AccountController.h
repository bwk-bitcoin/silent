#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <Qontrol>
#include <optional>
#include <silent.h>

class AccountWidget;

class AccountController : public QObject {
    Q_OBJECT

public:
    AccountController(const QString &account, AccountWidget *widget);
    auto init(const QString &account) -> void;
    auto screen(const QString &screen) -> std::optional<qontrol::Screen *>;
    virtual auto loadPanels() -> void;
    virtual auto getCoins() -> rust::Vec<RustCoin>;
    virtual auto getPaymentHistory() -> rust::Vec<RustTx>;
    virtual auto getSpAddress() -> rust::String;
    virtual auto newSegwitAddr() -> rust::String;
    virtual auto newTaprootAddr() -> rust::String;
    virtual auto hasSubAccounts() -> bool;
    virtual auto getDescriptor() -> rust::String;
    virtual auto addressVerifyInfo(const QString &address) -> AddressVerifyInfo;
    auto coins() -> qontrol::Screen *;
    auto getAccount() -> std::optional<rust::Box<Account>> &;

signals:
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);
    void newAddress(rust::String addr);
    void scanProgress(uint32_t height, uint32_t tip);
    void waitingForBlocks(uint32_t tip_height);
    void scanError(rust::String error);
    void scannerStateChanged(bool running);
    void electrumConnected(QString address);
    void electrumDisconnected();
    void notificationReceived(Notification notif);
    // Emitted once the background teardown is done and the notification thread
    // has exited, so the owner can safely delete the widget.
    void stopped();

public slots:
    auto loadPanel(const QString &name) -> void;
    auto insertPanel(qontrol::Panel *panel) -> void;
    auto pollCoins() -> void;
    auto handleNotification(Notification notif) -> void;
    virtual auto simulateTx(TransactionTemplate tx) -> TransactionSimulation;
    auto updateCoinLabel(const QString &outpoint, const QString &label) -> void;
    auto startScanner() -> void;
    auto stop() -> void;

    // Screen button actions
    auto historyClicked() -> void;
    auto coinsClicked() -> void;
    auto sendClicked() -> void;
    auto receiveClicked() -> void;
    auto settingsClicked() -> void;

    [[nodiscard]] auto isScannerRunning() const -> bool {
        return m_scanner_running;
    }

    [[nodiscard]] auto etaSecs() const -> uint64_t;

protected:
    // Construct without FFI init, for a mock subclass (dummy data, no account).
    explicit AccountController(AccountWidget *widget);

private:
    QPointer<qontrol::Panel> m_current_panel;
    QHash<QString, qontrol::Panel *> m_panels;
    AccountWidget *m_widget;
    std::optional<rust::Box<Account>> m_account = std::nullopt;
    QThread *m_notif_thread = nullptr;
    rust::Box<SyncEstimator> m_estimator = new_sync_estimator();
    bool m_init = false;
    bool m_stopping = false;
    bool m_scanner_running = false;
    int m_electrum_connected_count = 0;
    int m_electrum_expected_count = 0;
};
