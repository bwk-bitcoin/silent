#include "Settings.h"
#include "AccountController.h"
#include "AppController.h"
#include "Column.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/I18nManager.h"
#include "i18n/Tr.h"
#include "views/utils.h"
#include <Qontrol>
#include <common.h>
#include <qlogging.h>
#include <qnamespace.h>
#include <qthread.h>

namespace view {

using catalog::Button;
using catalog::ComboBox;
using catalog::FieldGroup;
using catalog::FoldSection;
using catalog::Input;
using catalog::Label;

Settings::Settings(AccountController *ctrl) {
    m_controller = ctrl;
    if (m_controller == nullptr) {
        return;
    }
    this->init();
    this->doConnect();
    this->view();
    fetchBackendInfo();
}

void Settings::init() {
    // Load current config values
    auto &accountOpt = m_controller->getAccount();
    if (accountOpt.has_value()) {
        auto accountName = accountOpt.value()->name();
        auto config = config_from_file(accountName);

        if (!config->is_ok()) {
            qWarning() << "Settings: cannot load config for account"
                       << QString::fromStdString(std::string(accountName.c_str())) << ":"
                       << QString::fromStdString(std::string(config->get_error().c_str()));
        } else {
            // Store config values to populate UI later
            m_current_url = QString::fromStdString(std::string(config->get_blindbit_url().c_str()));
            m_current_electrum_url =
                QString::fromStdString(std::string(config->get_electrum_url().c_str()));
            m_current_network = config->get_network();
        }
    }

    // Create widgets
    m_blindbit_url_input = new Input;
    m_blindbit_url_input->setWidth(Size::XL);
    m_blindbit_url_input->setPlaceholderText(TR("settings-placeholder-blindbit"));
    m_blindbit_url_input->setText(m_current_url);
    m_blindbit_url_input->setEnabled(!m_controller->isScannerRunning());

    m_test_btn = catalog::btnTest();
    m_testing_btn = catalog::btnTesting();
    m_testing_btn->setEnabled(false);
    m_test_ok_btn = catalog::btnTestOk();

    m_electrum_url_input = new Input;
    m_electrum_url_input->setWidth(Size::XL);
    m_electrum_url_input->setPlaceholderText(TR("settings-placeholder-electrum"));
    m_electrum_url_input->setText(m_current_electrum_url);
    m_electrum_url_input->setEnabled(!m_controller->isScannerRunning());

    m_test_electrum_btn = catalog::btnTest();
    m_testing_electrum_btn = catalog::btnTesting();
    m_testing_electrum_btn->setEnabled(false);
    m_test_ok_electrum_btn = catalog::btnTestOk();

    m_network_selector = new ComboBox;
    m_network_selector->addItem("Regtest", static_cast<int>(Network::Regtest));
    m_network_selector->addItem("Signet", static_cast<int>(Network::Signet));
    m_network_selector->addItem("Testnet", static_cast<int>(Network::Testnet));
    m_network_selector->addItem("Bitcoin", static_cast<int>(Network::Bitcoin));
    m_network_selector->setWidth(Size::M);

    int index = m_network_selector->findData(static_cast<int>(m_current_network));
    if (index != -1) {
        m_network_selector->setCurrentIndex(index);
    }
    m_network_selector->setEnabled(true);

    m_info_network_label = new Label("--");
    m_info_height_label = new Label("--");
    m_capabilities = new Label("--");

    m_language_selector = new ComboBox;
    m_language_selector->addItem("English", "en");
    m_language_selector->addItem("Francais", "fr");
    m_language_selector->addItem("Italiano", "it");
    m_language_selector->addItem("Deutsch", "de");
    m_language_selector->addItem("Portugues", "pt");
    m_language_selector->addItem("Espanol", "es");
    m_language_selector->setWidth(Size::M);

    int localeIndex = m_language_selector->findData(i18n::I18nManager::get()->selectedLocale());
    if (localeIndex == -1) {
        localeIndex = m_language_selector->findData("en");
    }
    if (localeIndex != -1) {
        m_language_selector->setCurrentIndex(localeIndex);
    }

    m_apply_language_btn = new Button;
    m_language_status_label = new Label;

    m_save_btn = catalog::btnSaveSettings();
    m_connect_blindbit_btn = catalog::btnConnectBlindbit();
    m_disconnect_blindbit_btn = catalog::btnDisconnectBlindbit();
    m_connect_electrum_btn = catalog::btnConnectElectrum();
    m_disconnect_electrum_btn = catalog::btnDisconnectElectrum();
    showTestState(m_test_btn, m_testing_btn, m_test_ok_btn, TestState::Untested);
    showTestState(m_test_electrum_btn, m_testing_electrum_btn, m_test_ok_electrum_btn,
                  TestState::Untested);

    retranslateUi();
    updateButtons();
}

void Settings::doConnect() {
    connect(m_save_btn, &QPushButton::clicked, this, &Settings::onActionSave, qontrol::UNIQUE);
    connect(m_connect_blindbit_btn, &QPushButton::clicked, this, &Settings::onActionToggleBlindbit,
            qontrol::UNIQUE);
    connect(m_disconnect_blindbit_btn, &QPushButton::clicked, this,
            &Settings::onActionToggleBlindbit, qontrol::UNIQUE);
    connect(m_connect_electrum_btn, &QPushButton::clicked, this, &Settings::onActionToggleElectrum,
            qontrol::UNIQUE);
    connect(m_disconnect_electrum_btn, &QPushButton::clicked, this,
            &Settings::onActionToggleElectrum, qontrol::UNIQUE);
    connect(m_test_btn, &QPushButton::clicked, this, &Settings::onActionTestBackend,
            qontrol::UNIQUE);
    connect(m_test_ok_btn, &QPushButton::clicked, this, &Settings::onActionTestBackend,
            qontrol::UNIQUE);
    connect(m_controller, &AccountController::scannerStateChanged, this,
            &Settings::onUpdateBlindbitToggleButton, qontrol::UNIQUE);
    connect(m_controller, &AccountController::electrumConnected, this,
            &Settings::onUpdateElectrumToggleButton, qontrol::UNIQUE);
    connect(m_controller, &AccountController::electrumDisconnected, this,
            &Settings::onUpdateElectrumToggleButton, qontrol::UNIQUE);
    connect(m_controller, &AccountController::scanProgress, this, &Settings::onScanProgress,
            qontrol::UNIQUE);
    connect(m_blindbit_url_input, &QLineEdit::textChanged, this, &Settings::invalidateBackendTest,
            qontrol::UNIQUE);
    connect(m_test_electrum_btn, &QPushButton::clicked, this, &Settings::onActionTestElectrum,
            qontrol::UNIQUE);
    connect(m_test_ok_electrum_btn, &QPushButton::clicked, this, &Settings::onActionTestElectrum,
            qontrol::UNIQUE);
    connect(m_apply_language_btn, &QPushButton::clicked, this, &Settings::onActionApplyLanguage,
            qontrol::UNIQUE);
    connect(m_electrum_url_input, &QLineEdit::textChanged, this, &Settings::invalidateElectrumTest,
            qontrol::UNIQUE);
    connect(this, &Settings::backendInfoReady, this, &Settings::onBackendInfoReady,
            qontrol::UNIQUE);
    connect(this, &Settings::electrumTestReady, this, &Settings::onElectrumTestReady,
            qontrol::UNIQUE);
}

void Settings::onActionSave() {
    qDebug() << "Settings::onActionSave()";

    auto &accountOpt = m_controller->getAccount();
    if (m_controller == nullptr || !accountOpt.has_value()) {
        auto *modal = new qontrol::Modal(TR("common-error"), TR("settings-no-account-loaded"));
        AppController::execModal(modal);
        return;
    }

    // Get account name
    auto accountName = accountOpt.value()->name();

    // Load existing config
    auto config = config_from_file(accountName);

    if (!config->is_ok()) {
        qCritical() << "Settings: cannot save, config load failed for account"
                    << QString::fromStdString(std::string(accountName.c_str())) << ":"
                    << QString::fromStdString(std::string(config->get_error().c_str()));
        auto *modal = new qontrol::Modal(TR("common-error"), TR("settings-no-account-loaded"));
        AppController::execModal(modal);
        return;
    }

    // Update BlindBit URL
    auto url = m_blindbit_url_input->text().toStdString();
    config->set_blindbit_url(rust::String(url));

    // Update Electrum URL
    auto electrumUrl = m_electrum_url_input->text().trimmed().toStdString();
    config->set_electrum_url(rust::String(electrumUrl));

    // Update network
    int networkIndex = m_network_selector->currentIndex();
    auto network = static_cast<Network>(m_network_selector->itemData(networkIndex).toInt());
    config->set_network(network);

    // Save to file
    config->to_file();

    m_current_url = m_blindbit_url_input->text();

    emit configSaved();
}

void Settings::onActionToggleBlindbit() {
    auto &accountOpt = m_controller->getAccount();
    if (m_controller == nullptr || !accountOpt.has_value()) {
        auto *modal = new qontrol::Modal(TR("common-error"), TR("settings-no-account-loaded"));
        AppController::execModal(modal);
        return;
    }

    if (m_controller->isScannerRunning()) {
        m_disconnect_blindbit_btn->setEnabled(false);
        accountOpt.value()->stop_scanner();
        clearBackendInfo();
    } else {
        if (!accountOpt.value()->start_scanner()) {
            auto *modal =
                new qontrol::Modal(TR("common-error"), TR("settings-failed-start-scanner"));
            AppController::execModal(modal);
        } else {
            fetchBackendInfo();
        }
    }
}

void Settings::onActionToggleElectrum() {
    auto &accountOpt = m_controller->getAccount();
    if (m_controller == nullptr || !accountOpt.has_value()) {
        auto *modal = new qontrol::Modal(TR("common-error"), TR("settings-no-account-loaded"));
        AppController::execModal(modal);
        return;
    }

    if (m_electrum_running) {
        m_disconnect_electrum_btn->setEnabled(false);
        accountOpt.value()->stop_electrum();
    } else {
        if (!accountOpt.value()->start_electrum()) {
            auto *modal =
                new qontrol::Modal(TR("common-error"), TR("settings-failed-start-electrum"));
            AppController::execModal(modal);
        }
    }
}

void Settings::onActionTestBackend() {
    auto url = m_blindbit_url_input->text().trimmed();
    if (url.isEmpty()) {
        auto *modal = new qontrol::Modal(TR("common-error"), TR("settings-blindbit-empty"));
        AppController::execModal(modal);
        return;
    }

    showTestState(m_test_btn, m_testing_btn, m_test_ok_btn, TestState::Testing);

    auto *thread = QThread::create([this, url = url.toStdString()]() -> void {
        auto info = ::get_backend_info(rust::String(url));
        emit backendInfoReady(info);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void Settings::showTestState(Button *test, Button *testing, Button *test_ok, TestState state) {
    test->setVisible(state == TestState::Untested);
    testing->setVisible(state == TestState::Testing);
    test_ok->setVisible(state == TestState::Ok);
}

void Settings::fetchBackendInfo() {
    auto url = m_blindbit_url_input->text().trimmed();
    if (url.isEmpty())
        return;

    auto *thread = QThread::create([this, url = url.toStdString()]() -> void {
        auto info = ::get_backend_info(rust::String(url));
        emit backendInfoReady(info);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void Settings::onBackendInfoReady(const BackendInfo &info) {
    if (!info.is_ok) {
        m_backend_verified = false;
        showTestState(m_test_btn, m_testing_btn, m_test_ok_btn, TestState::Untested);
        clearBackendInfo();
        updateButtons();
        auto rawError = QString::fromStdString(std::string(info.error));
        auto message =
            mapBackendErrorSummary(rawError) + "\n\n" + formatBackendErrorDetails(rawError);
        AppController::execModal(new qontrol::Modal(TR("settings-connection-failed"), message));
        return;
    }

    QString networkStr;
    switch (info.network) {
    case Network::Regtest:
        networkStr = TR("network-regtest");
        break;
    case Network::Signet:
        networkStr = TR("network-signet");
        break;
    case Network::Testnet:
        networkStr = TR("network-testnet");
        break;
    case Network::Bitcoin:
        networkStr = TR("network-bitcoin");
        break;
    }

    bool networkMatch = false;
    networkMatch = (info.network == m_current_network);
    m_blindbit_url_input->setText(QString::fromStdString(std::string(info.url)));

    m_backend_verified = networkMatch;
    showTestState(m_test_btn, m_testing_btn, m_test_ok_btn,
                  networkMatch ? TestState::Ok : TestState::Untested);
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

    if (!networkMatch) {
        AppController::execModal(new qontrol::Modal(TR("settings-network-mismatch"),
                                                    TR("settings-network-mismatch-message")
                                                        .arg(networkStr)
                                                        .arg(m_network_selector->currentText())));
    }
}

void Settings::invalidateBackendTest() {
    m_backend_verified = false;
    showTestState(m_test_btn, m_testing_btn, m_test_ok_btn, TestState::Untested);
    clearBackendInfo();
    updateButtons();
}

void Settings::onActionTestElectrum() {
    auto addr = m_electrum_url_input->text().trimmed();
    if (addr.isEmpty()) {
        auto *modal = new qontrol::Modal(TR("common-error"), TR("settings-electrum-empty"));
        AppController::execModal(modal);
        return;
    }

    showTestState(m_test_electrum_btn, m_testing_electrum_btn, m_test_ok_electrum_btn,
                  TestState::Testing);

    auto *thread = QThread::create([this, addr = addr.toStdString()]() -> void {
        auto result = ::test_electrum(rust::String(addr));
        emit electrumTestReady(result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void Settings::onElectrumTestReady(const ConnectionResult &result) {
    if (!result.is_ok) {
        m_electrum_verified = false;
        showTestState(m_test_electrum_btn, m_testing_electrum_btn, m_test_ok_electrum_btn,
                      TestState::Untested);
        updateButtons();
        auto rawError = QString::fromStdString(std::string(result.error));
        auto message =
            mapBackendErrorSummary(rawError) + "\n\n" + formatBackendErrorDetails(rawError);
        AppController::execModal(new qontrol::Modal(TR("settings-electrum-test-failed"), message));
        return;
    }

    // Before setting verified: setText fires textChanged -> invalidateElectrumTest.
    m_electrum_url_input->setText(QString::fromStdString(std::string(result.url)));

    m_electrum_verified = true;
    showTestState(m_test_electrum_btn, m_testing_electrum_btn, m_test_ok_electrum_btn,
                  TestState::Ok);
    updateButtons();
}

void Settings::invalidateElectrumTest() {
    m_electrum_verified = false;
    showTestState(m_test_electrum_btn, m_testing_electrum_btn, m_test_ok_electrum_btn,
                  TestState::Untested);
    updateButtons();
}

void Settings::clearBackendInfo() {
    m_info_network_label->setText("--");
    m_info_height_label->setText("--");
    m_capabilities->setText("--");
    m_current_height = 0;
}

void Settings::updateButtons() {
    bool blindbitRunning = m_controller->isScannerRunning();
    m_save_btn->setEnabled(m_backend_verified && m_electrum_verified);
    m_connect_blindbit_btn->setVisible(!blindbitRunning);
    m_disconnect_blindbit_btn->setVisible(blindbitRunning);
    m_connect_blindbit_btn->setEnabled(m_backend_verified);
    m_disconnect_blindbit_btn->setEnabled(true);
    m_connect_electrum_btn->setVisible(!m_electrum_running);
    m_disconnect_electrum_btn->setVisible(m_electrum_running);
    m_connect_electrum_btn->setEnabled(m_electrum_verified);
    m_disconnect_electrum_btn->setEnabled(true);
    m_blindbit_url_input->setEnabled(!blindbitRunning);
    m_test_btn->setEnabled(!blindbitRunning);
    m_test_ok_btn->setEnabled(!blindbitRunning);
    m_electrum_url_input->setEnabled(!m_electrum_running);
    m_test_electrum_btn->setEnabled(!m_electrum_running);
    m_test_ok_electrum_btn->setEnabled(!m_electrum_running);
}

void Settings::onScanProgress(uint32_t height, [[maybe_unused]] uint32_t tip) {
    if (height > m_current_height) {
        m_current_height = height;
        m_info_height_label->setText(QString::number(height));
    }
}

void Settings::onUpdateBlindbitToggleButton([[maybe_unused]] bool running) {
    updateButtons();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void Settings::onUpdateElectrumToggleButton() {
    m_electrum_running = !m_electrum_running;
    updateButtons();
}

void Settings::onActionApplyLanguage() {
    auto locale = m_language_selector->currentData().toString();
    bool ok = i18n::I18nManager::get()->applyLocale(locale, true);
    m_language_status_label->setText(ok ? TR("settings-language-applied")
                                        : TR("settings-language-saved-restart"));
}

void Settings::view() {
    auto *urlControl = (new qontrol::Row)
                           ->spacing(resolve(Spacing::XS))
                           ->push(m_blindbit_url_input)
                           ->push(m_test_btn)
                           ->push(m_testing_btn)
                           ->push(m_test_ok_btn);
    auto *electrumControl = (new qontrol::Row)
                                ->spacing(resolve(Spacing::XS))
                                ->push(m_electrum_url_input)
                                ->push(m_test_electrum_btn)
                                ->push(m_testing_electrum_btn)
                                ->push(m_test_ok_electrum_btn);

    auto *connectionContent = (new FieldGroup)
                                  ->addField(TR("settings-blindbit-url"), urlControl)
                                  ->addField(TR("settings-electrum-server"), electrumControl)
                                  ->addField(TR("settings-network"), m_network_selector);
    auto *connectionSection = new FoldSection(TR("settings-connection"));
    connectionSection->setContent(connectionContent);

    auto *infoContent = (new FieldGroup)
                            ->addField(TR("settings-network"), m_info_network_label)
                            ->addField(TR("settings-block-height"), m_info_height_label)
                            ->addField(TR("settings-capabilities"), m_capabilities, true);
    auto *infoSection = new FoldSection(TR("settings-backend-info"));
    infoSection->setExpanded(false);
    infoSection->setContent(infoContent);

    auto *buttonRow = (new qontrol::Row)
                          ->pushSpacer()
                          ->push(m_connect_blindbit_btn)
                          ->push(m_disconnect_blindbit_btn)
                          ->pushSpacer(resolve(Spacing::S))
                          ->push(m_connect_electrum_btn)
                          ->push(m_disconnect_electrum_btn)
                          ->pushSpacer(resolve(Spacing::S))
                          ->push(m_save_btn)
                          ->pushSpacer();

    auto *col = (new qontrol::Column)
                    ->pushSpacer(resolve(Spacing::XXL))
                    ->push(connectionSection)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::Separator)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(infoSection)
                    ->pushSpacer(resolve(Spacing::L))
                    ->push(buttonRow)
                    ->pushSpacer();

    auto *row = (new qontrol::Row)->pushSpacer()->push(col)->pushSpacer();

    setScreenContent(this, m_main_widget, dashboard(TR("settings-title"), row));
}

void Settings::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    qontrol::Screen::changeEvent(event);
}

void Settings::retranslateUi() {
    m_blindbit_url_input->setPlaceholderText(TR("settings-placeholder-blindbit"));
    m_electrum_url_input->setPlaceholderText(TR("settings-placeholder-electrum"));

    m_apply_language_btn->setText(TR("common-apply"));

    m_network_selector->setItemText(0, TR("network-regtest"));
    m_network_selector->setItemText(1, TR("network-signet"));
    m_network_selector->setItemText(2, TR("network-testnet"));
    m_network_selector->setItemText(3, TR("network-bitcoin"));
}

} // namespace view
