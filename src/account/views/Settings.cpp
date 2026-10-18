#include "account/views/Settings.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/MessageModal.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QHash>
#include <QThread>
#include <Qontrol>
#include <common.h>
#include <string>

namespace account::view {

using catalog::FieldGroup;
using catalog::FieldNote;
using catalog::FoldSection;

Settings::Settings(AccountBackend *backend) : m_backend(backend) {
    init();
    doConnect();
    view();
    if (auto *blindbit = row(Endpoint::Blindbit); blindbit != nullptr) {
        testEndpoint(*blindbit);
    }
}

void Settings::init() {
    for (auto endpoint : m_backend->endpoints()) {
        EndpointRow endpointRow;
        endpointRow.endpoint = endpoint;
        endpointRow.url = new catalog::Input;
        endpointRow.url->setWidth(Size::XL);
        endpointRow.url->setText(m_backend->endpointUrl(endpoint));
        endpointRow.test = catalog::btnTest();
        endpointRow.testing = catalog::btnTesting();
        endpointRow.testing->setEnabled(false);
        endpointRow.test_ok = catalog::btnTestOk();
        endpointRow.note = new FieldNote(QString(), FieldNote::Tone::Error);
        endpointRow.note->setVisible(false);
        bool blindbit = endpoint == Endpoint::Blindbit;
        endpointRow.connect =
            blindbit ? catalog::btnConnectBlindbit() : catalog::btnConnectElectrum();
        endpointRow.disconnect =
            blindbit ? catalog::btnDisconnectBlindbit() : catalog::btnDisconnectElectrum();
        showTestState(endpointRow, TestState::Untested);
        m_endpoints.append(endpointRow);
    }

    if (m_backend->networkEditable()) {
        m_network_selector = new catalog::ComboBox;
        m_network_selector->addItem("Regtest", static_cast<int>(Network::Regtest));
        m_network_selector->addItem("Signet", static_cast<int>(Network::Signet));
        m_network_selector->addItem("Testnet", static_cast<int>(Network::Testnet));
        m_network_selector->addItem("Bitcoin", static_cast<int>(Network::Bitcoin));
        m_network_selector->setWidth(Size::M);
        int index = m_network_selector->findData(static_cast<int>(m_backend->network()));
        if (index != -1) {
            m_network_selector->setCurrentIndex(index);
        }
    }

    m_info_network_label = new catalog::Label("--");
    m_info_height_label = new catalog::Label("--");
    m_capabilities = new catalog::Label("--");
    m_save_btn = catalog::btnSaveSettings();
    m_extras = m_backend->settingsExtras();

    retranslateUi();
    updateButtons();
}

void Settings::doConnect() {
    for (const auto &endpointRow : m_endpoints) {
        connect(endpointRow.url, &QLineEdit::textChanged, this, &Settings::onUrlEdited,
                qontrol::UNIQUE);
        connect(endpointRow.test, &QPushButton::clicked, this, &Settings::onTestClicked,
                qontrol::UNIQUE);
        connect(endpointRow.test_ok, &QPushButton::clicked, this, &Settings::onTestClicked,
                qontrol::UNIQUE);
        connect(endpointRow.connect, &QPushButton::clicked, this, &Settings::onConnectClicked,
                qontrol::UNIQUE);
        connect(endpointRow.disconnect, &QPushButton::clicked, this, &Settings::onDisconnectClicked,
                qontrol::UNIQUE);
    }
    connect(m_save_btn, &QPushButton::clicked, this, &Settings::onSaveClicked, qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::endpointStateChanged, this,
            &Settings::onEndpointStateChanged, qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::scanProgress, this, &Settings::onScanProgress,
            qontrol::UNIQUE);
    connect(this, &Settings::backendInfoReady, this, &Settings::onBackendInfoReady,
            qontrol::UNIQUE);
    connect(this, &Settings::electrumTestReady, this, &Settings::onElectrumTestReady,
            qontrol::UNIQUE);
}

void Settings::view() {
    auto *connectionContent = new FieldGroup;
    for (const auto &endpointRow : m_endpoints) {
        auto *control = (new qontrol::Row)
                            ->spacing(resolve(Spacing::XS))
                            ->push(endpointRow.url)
                            ->push(endpointRow.test)
                            ->push(endpointRow.testing)
                            ->push(endpointRow.test_ok);
        connectionContent->addField(label(endpointRow.endpoint), control)
            ->addNote(endpointRow.note);
    }
    if (m_network_selector != nullptr) {
        connectionContent->addField(TR("settings-network"), m_network_selector);
    }
    auto *connectionSection = new FoldSection(TR("settings-connection"));
    connectionSection->setContent(connectionContent);

    auto *col = (new qontrol::Column)->pushSpacer(resolve(Spacing::XXL))->push(connectionSection);

    if (row(Endpoint::Blindbit) != nullptr) {
        auto *infoContent = (new FieldGroup)
                                ->addField(TR("settings-network"), m_info_network_label)
                                ->addField(TR("settings-block-height"), m_info_height_label)
                                ->addField(TR("settings-capabilities"), m_capabilities, true);
        auto *infoSection = new FoldSection(TR("settings-backend-info"));
        infoSection->setExpanded(false);
        infoSection->setContent(infoContent);
        col->pushSpacer(resolve(Spacing::M))
            ->push(new catalog::Separator)
            ->pushSpacer(resolve(Spacing::M))
            ->push(infoSection);
    }

    auto *buttonRow = (new qontrol::Row)->pushSpacer();
    for (const auto &endpointRow : m_endpoints) {
        buttonRow->push(endpointRow.connect)
            ->push(endpointRow.disconnect)
            ->pushSpacer(resolve(Spacing::S));
    }
    buttonRow->push(m_save_btn)->pushSpacer();
    col->pushSpacer(resolve(Spacing::L))->push(buttonRow);

    if (m_extras != nullptr) {
        col->pushSpacer(resolve(Spacing::M))->push(m_extras);
    }
    col->pushSpacer();

    auto *centered = (new qontrol::Row)->pushSpacer()->push(col)->pushSpacer();
    setScreenContent(this, m_main_widget, dashboard(TR("settings-title"), centered));
}

void Settings::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    qontrol::Screen::changeEvent(event);
}

void Settings::retranslateUi() {
    for (const auto &endpointRow : m_endpoints) {
        endpointRow.url->setPlaceholderText(endpointRow.endpoint == Endpoint::Blindbit
                                                ? TR("settings-placeholder-blindbit")
                                                : TR("settings-placeholder-electrum"));
    }
    if (m_network_selector != nullptr) {
        m_network_selector->setItemText(0, TR("network-regtest"));
        m_network_selector->setItemText(1, TR("network-signet"));
        m_network_selector->setItemText(2, TR("network-testnet"));
        m_network_selector->setItemText(3, TR("network-bitcoin"));
    }
}

auto Settings::rowOf(const QObject *widget) -> EndpointRow * {
    for (auto &endpointRow : m_endpoints) {
        if (endpointRow.url == widget || endpointRow.test == widget ||
            endpointRow.test_ok == widget || endpointRow.connect == widget ||
            endpointRow.disconnect == widget) {
            return &endpointRow;
        }
    }
    return nullptr;
}

auto Settings::row(Endpoint endpoint) -> EndpointRow * {
    for (auto &endpointRow : m_endpoints) {
        if (endpointRow.endpoint == endpoint) {
            return &endpointRow;
        }
    }
    return nullptr;
}

auto Settings::label(Endpoint endpoint) -> QString {
    return endpoint == Endpoint::Blindbit ? TR("settings-blindbit-url")
                                          : TR("settings-electrum-server");
}

auto Settings::networkName(Network network) -> QString {
    switch (network) {
    case Network::Regtest:
        return TR("network-regtest");
    case Network::Signet:
        return TR("network-signet");
    case Network::Testnet:
        return TR("network-testnet");
    case Network::Bitcoin:
        return TR("network-bitcoin");
    }
    return {};
}

auto Settings::selectedNetwork() const -> Network {
    if (m_network_selector == nullptr) {
        return m_backend->network();
    }
    return static_cast<Network>(m_network_selector->currentData().toInt());
}

void Settings::showTestState(const EndpointRow &row, TestState state) {
    row.test->setVisible(state == TestState::Untested);
    row.testing->setVisible(state == TestState::Testing);
    row.test_ok->setVisible(state == TestState::Ok);
}

void Settings::showTestError(const EndpointRow &row, const QString &error) {
    row.note->setText(error);
    row.note->setVisible(!error.isEmpty());
}

void Settings::testEndpoint(const EndpointRow &row) {
    auto url = row.url->text().trimmed();
    if (url.isEmpty()) {
        return;
    }
    QThread *thread = nullptr;
    if (row.endpoint == Endpoint::Blindbit) {
        thread = QThread::create([this, url = url.toStdString()]() -> void {
            auto info = ::get_backend_info(rust::String(url));
            emit backendInfoReady(info);
        });
    } else {
        thread = QThread::create([this, url = url.toStdString()]() -> void {
            auto result = ::test_electrum(rust::String(url));
            emit electrumTestReady(result);
        });
    }
    m_backend->startWorker(thread);
}

void Settings::clearBackendInfo() {
    m_info_network_label->setText("--");
    m_info_height_label->setText("--");
    m_capabilities->setText("--");
    m_current_height = 0;
}

void Settings::updateButtons() {
    bool allVerified = true;
    for (const auto &endpointRow : m_endpoints) {
        bool running = m_backend->isEndpointRunning(endpointRow.endpoint);
        allVerified = allVerified && endpointRow.verified;
        endpointRow.connect->setVisible(!running);
        endpointRow.disconnect->setVisible(running);
        endpointRow.connect->setEnabled(endpointRow.verified);
        endpointRow.disconnect->setEnabled(true);
        endpointRow.url->setEnabled(!running);
        endpointRow.test->setEnabled(!running && !endpointRow.url->text().trimmed().isEmpty());
        endpointRow.test_ok->setEnabled(!running);
    }
    m_save_btn->setEnabled(allVerified);
}

// ===== Slots =====

void Settings::onSaveClicked() {
    QHash<Endpoint, QString> urls;
    for (const auto &endpointRow : m_endpoints) {
        urls.insert(endpointRow.endpoint, endpointRow.url->text().trimmed());
    }
    auto error = m_backend->saveSettings(urls, selectedNetwork());
    if (!error.isEmpty()) {
        AppController::execModal(new catalog::MessageModal(TR("common-error"), error));
    }
}

void Settings::onConnectClicked() {
    auto *endpointRow = rowOf(sender());
    if (endpointRow == nullptr) {
        return;
    }
    if (!m_backend->startEndpoint(endpointRow->endpoint)) {
        auto message = endpointRow->endpoint == Endpoint::Blindbit
                           ? TR("settings-failed-start-scanner")
                           : TR("settings-failed-start-electrum");
        AppController::execModal(new catalog::MessageModal(TR("common-error"), message));
        return;
    }
    if (endpointRow->endpoint == Endpoint::Blindbit) {
        testEndpoint(*endpointRow);
    }
}

void Settings::onDisconnectClicked() {
    auto *endpointRow = rowOf(sender());
    if (endpointRow == nullptr) {
        return;
    }
    endpointRow->disconnect->setEnabled(false);
    m_backend->stopEndpoint(endpointRow->endpoint);
    if (endpointRow->endpoint == Endpoint::Blindbit) {
        clearBackendInfo();
    }
}

void Settings::onTestClicked() {
    auto *endpointRow = rowOf(sender());
    if (endpointRow == nullptr || endpointRow->url->text().trimmed().isEmpty()) {
        return;
    }
    showTestError(*endpointRow, QString());
    showTestState(*endpointRow, TestState::Testing);
    testEndpoint(*endpointRow);
}

void Settings::onUrlEdited() {
    auto *endpointRow = rowOf(sender());
    if (endpointRow == nullptr) {
        return;
    }
    endpointRow->verified = false;
    showTestError(*endpointRow, QString());
    showTestState(*endpointRow, TestState::Untested);
    if (endpointRow->endpoint == Endpoint::Blindbit) {
        clearBackendInfo();
    }
    updateButtons();
}

void Settings::onEndpointStateChanged([[maybe_unused]] Endpoint endpoint) {
    updateButtons();
}

void Settings::onScanProgress(uint32_t height, [[maybe_unused]] uint32_t tip) {
    if (height > m_current_height) {
        m_current_height = height;
        m_info_height_label->setText(QString::number(height));
    }
}

void Settings::onBackendInfoReady(const BackendInfo &info) {
    auto *blindbit = row(Endpoint::Blindbit);
    if (blindbit == nullptr) {
        return;
    }
    if (!info.is_ok) {
        blindbit->verified = false;
        showTestState(*blindbit, TestState::Untested);
        clearBackendInfo();
        updateButtons();
        showTestError(*blindbit,
                      mapBackendErrorSummary(QString::fromStdString(std::string(info.error))));
        return;
    }

    auto networkStr = networkName(info.network);
    bool networkMatch = info.network == selectedNetwork();
    // Before setting verified: setText fires textChanged, which clears it.
    blindbit->url->setText(QString::fromStdString(std::string(info.url)));
    blindbit->verified = networkMatch;
    showTestState(*blindbit, networkMatch ? TestState::Ok : TestState::Untested);
    updateButtons();
    m_info_network_label->setText(networkStr);
    m_current_height = info.height;
    m_info_height_label->setText(QString::number(info.height));

    auto yesText = TR("common-yes");
    auto noText = TR("common-no");
    auto yn = [&yesText, &noText](bool value) -> QString { return value ? yesText : noText; };
    m_capabilities->setText(TR("settings-capabilities-template")
                                .arg(yn(info.tweaks_only))
                                .arg(yn(info.tweaks_full_basic))
                                .arg(yn(info.tweaks_full_with_dust_filter))
                                .arg(yn(info.tweaks_cut_through_with_dust_filter)));
    m_capabilities->setAlignment(Qt::AlignTop);

    QString mismatch;
    if (!networkMatch) {
        mismatch =
            TR("create-account-backend-network").arg(networkStr, networkName(selectedNetwork()));
    }
    showTestError(*blindbit, mismatch);
}

void Settings::onElectrumTestReady(const ConnectionResult &result) {
    auto *electrum = row(Endpoint::Electrum);
    if (electrum == nullptr) {
        return;
    }
    if (!result.is_ok) {
        electrum->verified = false;
        showTestState(*electrum, TestState::Untested);
        updateButtons();
        showTestError(*electrum,
                      mapBackendErrorSummary(QString::fromStdString(std::string(result.error))));
        return;
    }
    // Before setting verified: setText fires textChanged, which clears it.
    electrum->url->setText(QString::fromStdString(std::string(result.url)));
    electrum->verified = true;
    showTestState(*electrum, TestState::Ok);
    updateButtons();
}

} // namespace account::view
