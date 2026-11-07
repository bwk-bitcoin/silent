#pragma once

#include "account/AccountBackend.h"
#include "account/ElectrumReconnect.h"
#include "account/PanelCollection.h"

#include <QList>
#include <QString>
#include <QThread>
#include <Qontrol>
#include <memory>
#include <optional>
#include <silent.h>

class AccountWidget;

class AccountController : public AccountBackend {
    Q_OBJECT

public:
    AccountController(const QString &account, AccountWidget *widget);
    ~AccountController() override;
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
    auto settingsExtras() -> QWidget * override;
    [[nodiscard]] auto services() const -> account::ServiceContext * override;
    auto module() -> IModule * override;
    auto simulateTx(TransactionTemplate tx) -> TransactionSimulation override;
    // Sign, then Broadcast.
    auto sendSigning() -> account::SendSigning * override;
    auto coins() -> qontrol::Screen *;
    auto getAccount() -> std::optional<rust::Box<Account>> &;
    // stop() waits for the worker before it drops the account and before stopped()
    // lets the owner delete them.
    void startWorker(QThread *worker) override;
    void requestTxDetails(uint64_t req_id, const QString &txid) override;
    void editTxLabel(uint64_t req_id, const QString &txid, const QString &label) override;
    void editOutpointLabel(uint64_t req_id, const QString &outpoint, const QString &label) override;
    void requestEndpointTip(uint64_t req_id, Endpoint endpoint) override;
    void testAndSaveEndpoint(uint64_t req_id, Endpoint endpoint, const QString &url,
                             bool verify_certificate) override;
    void setEndpointConnected(uint64_t req_id, Endpoint endpoint, bool connected) override;

signals:
    void newAddress(rust::String addr);
    void waitingForBlocks(uint32_t tip_height);
    void scannerStateChanged(bool running);
    void electrumConnected(QString address);
    void electrumDisconnected();
    void notificationReceived(uint64_t generation, Notification notif);

public slots:
    auto loadPanel(const QString &name) -> void;
    auto insertPanel(qontrol::Panel *panel) -> void;
    auto pollCoins() -> void;
    auto handleNotification(Notification notif) -> void;
    void onNotificationReceived(uint64_t generation, Notification notif);
    void updateCoinLabel(const QString &outpoint, const QString &label) override;
    auto stop() -> void;
    void onWorkerFinished();
    void onEndpointFinished();
    void onAccountLoaded();
    // A lost Electrum connection starts again.
    void onElectrumReconnect();
    // The reconnect gave up: the user is told, the connection stays down.
    void onElectrumGaveUp(int losses);

    // Screen button actions
    auto historyClicked() -> void;
    auto coinsClicked() -> void;
    auto sendClicked() -> void;
    auto receiveClicked() -> void;
    auto settingsClicked() -> void;

    [[nodiscard]] auto etaSecs() const -> uint64_t;

protected:
    // Construct without FFI init, for a mock subclass (dummy data, no account).
    explicit AccountController(AccountWidget *widget);
    void listenNotifications();
    void changeEndpoint(uint64_t req_id, Endpoint endpoint, const QString &url,
                        bool verify_certificate, bool test, bool connected, bool retry = false,
                        bool startup = false);

private:
    account::PanelCollection m_panels{this};
    AccountWidget *m_widget;
    account::ServiceContext *m_services = nullptr;
    std::optional<rust::Box<Account>> m_account = std::nullopt;
    QThread *m_notif_thread = nullptr;
    uint64_t m_notif_generation = 0;
    QList<QThread *> m_workers;
    rust::Box<SyncEstimator> m_estimator = new_sync_estimator();
    bool m_init = false;
    bool m_panels_requested = false;
    QString m_requested_panel = QStringLiteral("history");
    std::shared_ptr<std::optional<rust::Box<Account>>> m_loading_account;
    bool m_stopping = false;
    bool m_scanner_running = false;
    bool m_electrum_running = false;
    account::ElectrumReconnect *m_reconnect = new account::ElectrumReconnect(this);
    int m_electrum_connected_count = 0;
    int m_electrum_expected_count = 0;
    std::shared_ptr<std::optional<rust::Box<Account>>> m_endpoint_account;
    std::shared_ptr<EndpointResult> m_endpoint_result;
    std::shared_ptr<bool> m_endpoint_replaced;
    Endpoint m_endpoint = Endpoint::Electrum;
    bool m_endpoint_busy = false;
    bool m_endpoint_retry = false;
    bool m_endpoint_startup = false;
    std::shared_ptr<BackendServer> m_endpoint_server;
    bool m_reconnect_pending = false;
    QList<Notification> m_endpoint_notifications;
    bool m_endpoint_stop_requested = false;
};
