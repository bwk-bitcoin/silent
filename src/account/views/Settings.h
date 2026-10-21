#pragma once

#include "account/AccountBackend.h"

#include <QEvent>
#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <cstdint>
#include <silent.h>

namespace catalog {
class Button;
class FieldNote;
class Input;
class Label;
class Toggle;
} // namespace catalog

class QTimer;

namespace account::view {

class BackendInfoReply : public QObject {
    Q_OBJECT

signals:
    void ready(uint64_t req_id, BackendInfo info);
};

// One row per server the account syncs from: a passing Test saves the server and
// leaves it disconnected, Connect waits for a saved server. BlindBit's capabilities
// under its row, then what the account adds below.
class Settings : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Settings(AccountBackend *backend);

public slots:
    void onTestClicked();
    void onConnectClicked();
    void onDisconnectClicked();
    void onUrlEdited();
    void onSslToggled(bool checked);
    void onVerifyToggled(bool checked);
    void onEndpointStateChanged(Endpoint endpoint);
    void onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                   const EndpointResult &result);
    void onEndpointTipReady(uint64_t req_id, Endpoint endpoint, const ElectrumTipResult &result);
    void onBackendInfoReady(uint64_t req_id, const BackendInfo &info);
    void onHeightTimeout();

protected:
    struct ServerRow {
        Endpoint endpoint = Endpoint::Electrum;
        catalog::Label *label = nullptr;
        catalog::Input *url = nullptr;
        // "Height N" while connected, empty otherwise.
        catalog::Label *height = nullptr;
        catalog::Button *test = nullptr;
        catalog::Button *testing = nullptr;
        catalog::Button *connect = nullptr;
        catalog::Button *disconnect = nullptr;
        // Why the last test, connect or disconnect failed.
        catalog::FieldNote *note = nullptr;
        // Electrum only.
        catalog::Toggle *ssl = nullptr;
        catalog::Toggle *verify = nullptr;
        // The Verify Certificate choice, kept while SSL is off.
        bool verify_wanted = true;
        QString saved_url;
        bool saved_verify = true;
        // The test, connect or disconnect in flight, 0 when none.
        uint64_t action_req = 0;
        bool testing_req = false;
        uint64_t tip_req = 0;
    };

    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    // The heights are refreshed while the screen shows.
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void retranslateUi();
    [[nodiscard]] static auto viewRow(const ServerRow &row) -> QWidget *;
    // The row `widget` (its input, toggles or buttons) belongs to, nullptr when none.
    [[nodiscard]] auto rowOf(const QObject *widget) -> ServerRow *;
    [[nodiscard]] auto row(Endpoint endpoint) -> ServerRow *;
    [[nodiscard]] static auto label(Endpoint endpoint) -> QString;
    [[nodiscard]] static auto networkName(Network network) -> QString;
    // What Test would save: the input, its scheme set by SSL when it states one.
    [[nodiscard]] static auto candidateUrl(const ServerRow &row) -> QString;
    [[nodiscard]] static auto candidateVerify(const ServerRow &row) -> bool;
    // The input or the transport differs from the saved server.
    [[nodiscard]] static auto isDirty(const ServerRow &row) -> bool;
    [[nodiscard]] auto isBusy() const -> bool;
    // Reads the saved server back from the account.
    void loadSaved(ServerRow &row);
    // Puts the saved server in the input and the toggles.
    static void showSaved(ServerRow &row);
    static void showError(const ServerRow &row, const QString &error);
    void refreshRows();
    void refreshRow(const ServerRow &row);
    void requestHeight(ServerRow &row);
    void requestCapabilities();
    void startAction(ServerRow &row, bool test);

private:
    AccountBackend *m_backend = nullptr;
    QWidget *m_main_widget = nullptr;
    QList<ServerRow> m_rows;
    catalog::Label *m_capabilities = nullptr;
    uint64_t m_info_req = 0;
    QTimer *m_height_timer = nullptr;
    QWidget *m_extras = nullptr;
};

} // namespace account::view
