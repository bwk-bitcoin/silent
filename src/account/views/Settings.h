#pragma once

#include "account/Services.h"
#include "interfaces/backend/types.h"

#include <QEvent>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <cstdint>
#include <optional>

namespace catalog {
class Button;
class EndpointRow;
class Label;
class Toggle;
} // namespace catalog

class QTimer;

namespace account::view {

// One row per server the account syncs from, as IBackendSettings lists them and presents
// them (extension::ENDPOINT_PRESENTATION): a passing Test saves the server and leaves it
// disconnected, Connect waits for a saved server. A server's capabilities under its row,
// then `extras`, what the account adds below, then Backup (BackupAccount). With `lock_connected`,
// the address of a server reached over a transport (SSL and certificate toggles) cannot change
// while it is connected.
class Settings : public qontrol::Screen {
    Q_OBJECT

public:
    Settings(const Services &services, QWidget *extras, bool lock_connected = false);
    ~Settings() override;

public slots:
    void onTestClicked();
    void onConnectClicked();
    void onDisconnectClicked();
    void onUrlEdited();
    void onSslToggled(bool checked);
    void onVerifyToggled(bool checked);
    void onEndpointsChanged();
    void onEndpointTested(backend::RequestId request_id);
    void onSettingsSaved(backend::RequestId request_id);
    void onBlockchainCompleted(backend::RequestId request_id);
    void onServiceError(backend::RequestId request_id, const QString &message);
    void onSettingsValue(backend::RequestId request_id, const QJsonValue &payload);
    void onSettingsEvent(const QString &operation, const QJsonValue &payload);
    void onHeightTimeout();
    void onBackupClicked();

protected:
    // What a row's request in flight is doing. A Test runs Testing, Saving, then
    // Disconnecting, which leaves the tested server disconnected.
    enum class Step : std::uint8_t { None, Testing, Saving, Disconnecting, Connecting };

    // How a server's row shows, as its provider says.
    struct Presentation {
        QString label;
        QString placeholder;
        QString connect;
        QString disconnect;
        // The SSL and certificate toggles.
        bool transport = false;
        // What the server serves, under its row.
        bool capabilities = false;
    };

    struct ServerRow {
        backend::Endpoint endpoint;
        Presentation presentation;
        catalog::Label *label = nullptr;
        catalog::EndpointRow *field = nullptr;
        // "Height N" while connected, empty otherwise.
        catalog::Label *height = nullptr;
        catalog::Button *connect = nullptr;
        catalog::Button *disconnect = nullptr;
        // Electrum only.
        catalog::Toggle *ssl = nullptr;
        catalog::Toggle *verify = nullptr;
        // The Verify Certificate choice, kept while SSL is off.
        bool verify_wanted = true;
        QString saved_url;
        bool saved_verify = true;
        // The test, connect or disconnect in flight, 0 when none.
        backend::RequestId action_req = 0;
        Step step = Step::None;
        // The server the Test in flight checks, saved once it passes.
        backend::Endpoint tested;
        // The tested server is saved.
        bool saved = false;
        uint64_t draft_revision = 0;
        uint64_t action_revision = 0;
        backend::RequestId tip_req = 0;
        // The connection as last read, so a state update that leaves it is no change.
        bool seen_connected = false;
        QString seen_error;
    };

    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    // The heights are refreshed while the screen shows.
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void retranslateUi();
    // What the account's backup holds and what opens it, its account's own kind.
    [[nodiscard]] auto backupSummary() const -> QString;
    void connectServices() const;
    [[nodiscard]] auto presentationOf(const backend::Endpoint &endpoint) const -> Presentation;
    // Puts the provider's texts on the rows.
    void showPresentation();
    [[nodiscard]] static auto viewRow(const ServerRow &row) -> QWidget *;
    // The row `widget` (its input, toggles or buttons) belongs to, nullptr when none.
    [[nodiscard]] auto rowOf(const QObject *widget) -> ServerRow *;
    [[nodiscard]] auto row(const QString &id) -> ServerRow *;
    // The row whose request `request_id` is, its action or its height.
    [[nodiscard]] auto rowOfRequest(backend::RequestId request_id) -> ServerRow *;
    [[nodiscard]] auto connection(const QString &id) const -> std::optional<backend::Connection>;
    [[nodiscard]] auto isRunning(const ServerRow &row) const -> bool;
    // What Test would save: the input, its scheme set by SSL when it states one.
    [[nodiscard]] static auto candidateUrl(const ServerRow &row) -> QString;
    [[nodiscard]] static auto candidateVerify(const ServerRow &row) -> bool;
    // The input or the transport differs from the saved server.
    [[nodiscard]] static auto isDirty(const ServerRow &row) -> bool;
    [[nodiscard]] auto isBusy() const -> bool;
    // Reads the saved server back from the account.
    static void loadSaved(ServerRow &row, const backend::Endpoint &saved);
    // Puts the saved server in the input and the toggles.
    static void showSaved(ServerRow &row);
    static void showError(const ServerRow &row, const QString &error);
    void refreshRows();
    void refreshRow(const ServerRow &row);
    void requestHeight(ServerRow &row);
    void requestCapabilities();
    void refreshEndpoints(bool busy_changed);
    void startAction(ServerRow &row, Step step);
    // The Test of `row` is done: passed when `error` is empty.
    void finishTest(ServerRow &row, const QString &error);

private:
    Services m_services;
    bool m_lock_connected = false;
    // The provider's presentation of each endpoint, by id.
    QJsonObject m_presentation;
    backend::RequestId m_presentation_req = 0;
    QWidget *m_main_widget = nullptr;
    QList<ServerRow> m_rows;
    catalog::Label *m_capabilities = nullptr;
    backend::RequestId m_info_req = 0;
    backend::RequestId m_busy_req = 0;
    // An endpoint operation runs, as the provider reports it (extension::ENDPOINTS_BUSY).
    bool m_endpoints_busy = false;
    QTimer *m_height_timer = nullptr;
    QWidget *m_extras = nullptr;
    catalog::Label *m_backup_text = nullptr;
    catalog::Button *m_backup = nullptr;
};

} // namespace account::view
