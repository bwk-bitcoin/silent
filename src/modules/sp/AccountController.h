#pragma once

#include "account/AccountBackend.h"
#include "account/ElectrumReconnect.h"
#include "account/PanelCollection.h"
#include "host/ServiceAssembly.h"

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
    virtual auto getSpAddress() -> rust::String;
    auto network() -> Network override;
    virtual auto hasSegwit() -> bool;
    virtual auto hasTaproot() -> bool;
    virtual auto getDescriptor() -> rust::String;
    // BlindBit, then Electrum.
    auto endpoints() -> QList<Endpoint> override;
    auto endpointUrl(Endpoint endpoint) -> QString override;
    [[nodiscard]] auto verifiesCertificate() const -> bool override;
    [[nodiscard]] auto isEndpointRunning(Endpoint endpoint) const -> bool override;
    [[nodiscard]] auto services() const -> account::ServiceContext * override;
    auto module() -> IModule * override;
    auto coins() -> qontrol::Screen *;
    auto getAccount() -> std::optional<rust::Box<Account>> &;
    // stop() waits for the worker before it drops the account and before stopped()
    // lets the owner delete them.
    void startWorker(QThread *worker) override;
    void requestTxDetails(uint64_t req_id, const QString &txid) override;
    void requestEndpointTip(uint64_t req_id, Endpoint endpoint) override;
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
    auto stop() -> void;
    void onWorkerFinished();
    void onEndpointFinished();
    void onAccountLoaded();
    // A lost Electrum connection starts again.
    void onElectrumReconnect();
    // The reconnect gave up: the user is told, the connection stays down.
    void onElectrumGaveUp(int losses);
    // A reconnect held back by a settings test or save runs.
    void onSettingsIdle();
    // A provider of the account's services stops: the panels using them go first.
    void onServicesRetired();
    // The first funds read answered: the panels reading them are built.
    void onFundsAnswered();

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
    void changeEndpoint(uint64_t req_id, Endpoint endpoint, bool connected, bool retry = false,
                        bool startup = false);
    // The account's services, assembled once from this module. False when the host
    // refuses them, or once they retired.
    auto assembleServices() -> bool;

private:
    // Declared before the panels, which use it: they go first.
    std::unique_ptr<host::ServiceAssembly> m_assembly;
    bool m_services_retired = false;
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
