#pragma once

#include "account/AccountBackend.h"

#include <QEvent>
#include <QList>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <cstdint>
#include <silent.h>

namespace catalog {
class Button;
class ComboBox;
class FieldNote;
class Input;
class Label;
} // namespace catalog

namespace account::view {

// One row per server the account syncs from, each tested before Save; the network
// when the account lets it change; the BlindBit server's info when it has one; then
// what the account adds below.
class Settings : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Settings(AccountBackend *backend);

signals:
    void backendInfoReady(BackendInfo info);
    void electrumTestReady(ConnectionResult result);

public slots:
    void onSaveClicked();
    void onConnectClicked();
    void onDisconnectClicked();
    void onTestClicked();
    void onUrlEdited();
    void onEndpointStateChanged(Endpoint endpoint);
    void onScanProgress(uint32_t height, uint32_t tip);
    void onBackendInfoReady(const BackendInfo &info);
    void onElectrumTestReady(const ConnectionResult &result);

protected:
    enum class TestState : std::uint8_t { Untested, Testing, Ok };

    struct EndpointRow {
        Endpoint endpoint = Endpoint::Electrum;
        catalog::Input *url = nullptr;
        // The Test button cycles Test, Testing… and ✓ Test.
        catalog::Button *test = nullptr;
        catalog::Button *testing = nullptr;
        catalog::Button *test_ok = nullptr;
        // The reason the last test failed, under the field.
        catalog::FieldNote *note = nullptr;
        catalog::Button *connect = nullptr;
        catalog::Button *disconnect = nullptr;
        bool verified = false;
    };

    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    // The row `widget` (its input or one of its buttons) belongs to, nullptr when none.
    [[nodiscard]] auto rowOf(const QObject *widget) -> EndpointRow *;
    [[nodiscard]] auto row(Endpoint endpoint) -> EndpointRow *;
    [[nodiscard]] static auto label(Endpoint endpoint) -> QString;
    [[nodiscard]] static auto networkName(Network network) -> QString;
    [[nodiscard]] auto selectedNetwork() const -> Network;
    static void showTestState(const EndpointRow &row, TestState state);
    static void showTestError(const EndpointRow &row, const QString &error);
    // Tests `row`'s url on a worker; the answer comes back as backendInfoReady or
    // electrumTestReady.
    void testEndpoint(const EndpointRow &row);
    void clearBackendInfo();
    void updateButtons();

private:
    AccountBackend *m_backend = nullptr;
    QWidget *m_main_widget = nullptr;
    QList<EndpointRow> m_endpoints;
    catalog::ComboBox *m_network_selector = nullptr;
    catalog::Label *m_info_network_label = nullptr;
    catalog::Label *m_info_height_label = nullptr;
    catalog::Label *m_capabilities = nullptr;
    catalog::Button *m_save_btn = nullptr;
    QWidget *m_extras = nullptr;
    uint32_t m_current_height = 0;
};

} // namespace account::view
