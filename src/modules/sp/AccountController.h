#pragma once

#include "account/AccountBackend.h"
#include "account/ElectrumReconnect.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QThread>
#include <Qontrol>
#include <optional>
#include <silent.h>

class AccountWidget;

class AccountController : public AccountBackend {
    Q_OBJECT

public:
    AccountController(const QString &account, AccountWidget *widget);
    auto init(const QString &account) -> void;
    auto screen(const QString &screen) -> std::optional<qontrol::Screen *>;
    virtual auto loadPanels() -> void;
    auto getCoins() -> rust::Vec<RustCoin> override;
    auto getPaymentHistory() -> rust::Vec<RustTx> override;
    virtual auto getSpAddress() -> rust::String;
    virtual auto newSegwitAddr() -> rust::String;
    virtual auto newTaprootAddr() -> rust::String;
    auto network() -> Network override;
    virtual auto hasSegwit() -> bool;
    virtual auto hasTaproot() -> bool;
    // The silent payment address, then the segwit and taproot sub-accounts it has.
    auto addressKinds() -> QList<AddressKind> override;
    auto newAddress(AddressKind kind) -> QString override;
    virtual auto getDescriptor() -> rust::String;
    auto addressVerifyInfo(const QString &address) -> AddressVerifyInfo override;
    // BlindBit, then Electrum.
    auto endpoints() -> QList<Endpoint> override;
    auto endpointUrl(Endpoint endpoint) -> QString override;
    [[nodiscard]] auto verifiesCertificate() const -> bool override;
    [[nodiscard]] auto isEndpointRunning(Endpoint endpoint) const -> bool override;
    auto startEndpoint(Endpoint endpoint) -> bool override;
    void stopEndpoint(Endpoint endpoint) override;
    [[nodiscard]] auto networkEditable() const -> bool override;
    auto saveSettings(const QHash<Endpoint, QString> &urls, Network network) -> QString override;
    auto settingsExtras() -> QWidget * override;
    auto simulateTx(TransactionTemplate tx) -> TransactionSimulation override;
    // Sign, then Broadcast.
    auto sendSigning() -> account::SendSigning * override;
    auto coins() -> qontrol::Screen *;
    auto getAccount() -> std::optional<rust::Box<Account>> &;
    // stop() waits for the worker before it drops the account and before stopped()
    // lets the owner delete them.
    void startWorker(QThread *worker) override;

signals:
    void newAddress(rust::String addr);
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
    void updateCoinLabel(const QString &outpoint, const QString &label) override;
    auto startScanner() -> void;
    auto stop() -> void;
    void onWorkerFinished();
    // A lost Electrum connection starts again.
    void onElectrumReconnect();

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
    QList<QThread *> m_workers;
    rust::Box<SyncEstimator> m_estimator = new_sync_estimator();
    bool m_init = false;
    bool m_stopping = false;
    bool m_scanner_running = false;
    bool m_electrum_running = false;
    account::ElectrumReconnect *m_reconnect = new account::ElectrumReconnect(this);
    int m_electrum_connected_count = 0;
    int m_electrum_expected_count = 0;
};
