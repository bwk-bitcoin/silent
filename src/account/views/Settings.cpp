#include "account/views/Settings.h"

#include "account/Extensions.h"
#include "account/RequestId.h"
#include "account/views/BackupAccount.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Section.h"
#include "catalog/display/Badge.h"
#include "catalog/display/InfoTip.h"
#include "catalog/display/Label.h"
#include "catalog/form/EndpointRow.h"
#include "catalog/form/Field.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/Toggle.h"
#include "i18n/Tr.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/settings.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QJsonObject>
#include <QSizePolicy>
#include <QTimer>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <utility>

namespace {

// How often the server heights are asked while Settings shows.
constexpr int HEIGHT_REFRESH_MS = 30000;

const QString K_SSL_SCHEME = QStringLiteral("ssl://");
const QString K_TCP_SCHEME = QStringLiteral("tcp://");
const QString K_SCHEME_SEPARATOR = QStringLiteral("://");

} // namespace

namespace account::view {

Settings::Settings(const Services &services, QWidget *extras, bool lock_connected)
    : m_services(services),
      m_lock_connected(lock_connected),
      m_extras(extras) {
    // First: the providers answer the requests init() sends during the call.
    connectServices();
    init();
    doConnect();
    view();
    requestCapabilities();
}

Settings::~Settings() {
    for (const auto &serverRow : m_rows) {
        delete serverRow.field;
    }
}

void Settings::init() {
    // The built-in providers answer during the call: their rows take shape from it.
    m_presentation_req = nextRequestId();
    m_services.settings->value(m_presentation_req, extension::ENDPOINT_PRESENTATION, QJsonValue());
    for (const auto &endpoint : m_services.settings->settings().endpoints) {
        ServerRow serverRow;
        serverRow.endpoint = endpoint;
        serverRow.presentation = presentationOf(endpoint);
        serverRow.label = new catalog::Label(catalog::LabelRole::Eyebrow);
        serverRow.label->setTone(catalog::LabelTone::Muted);
        serverRow.height = new catalog::Label(catalog::LabelRole::Mono);
        serverRow.height->setTone(catalog::LabelTone::Secondary);
        // A fixed slot, so stacked rows' inputs share one width.
        serverRow.height->setFixedWidth(resolve(Size::XS));
        serverRow.connect = catalog::btnEndpoint(serverRow.presentation.connect);
        serverRow.disconnect = catalog::btnEndpoint(serverRow.presentation.disconnect);
        auto *controls = (new qontrol::Row)
                             ->spacing(resolve(Spacing::S))
                             ->push(serverRow.height)
                             ->push(serverRow.connect)
                             ->push(serverRow.disconnect);
        serverRow.field = new catalog::EndpointRow(controls);
        if (serverRow.presentation.transport) {
            serverRow.ssl = new catalog::Toggle;
            serverRow.verify = new catalog::Toggle;
        }
        loadSaved(serverRow, endpoint);
        showSaved(serverRow);
        auto connection = this->connection(endpoint.id);
        if (connection.has_value()) {
            serverRow.seen_connected = connection->connected;
            serverRow.seen_error = connection->error;
        }
        if (!serverRow.seen_error.isEmpty()) {
            showError(serverRow, serverRow.seen_error);
        }
        m_rows.append(serverRow);
    }

    m_capabilities = new catalog::Label("--");
    m_capabilities->setAlignment(Qt::AlignTop);
    m_backup_text = new catalog::Label(catalog::LabelRole::Body);
    m_backup_text->setTone(catalog::LabelTone::Secondary);
    m_backup_text->setWordWrap(true);
    m_backup_text->setText(TR("settings-backup-text"));
    m_backup = catalog::btnBackupAccount();
    m_height_timer = new QTimer(this);
    m_height_timer->setInterval(HEIGHT_REFRESH_MS);

    showPresentation();
    refreshRows();
}

void Settings::doConnect() {
    for (const auto &serverRow : m_rows) {
        connect(serverRow.field->input(), &QLineEdit::textEdited, this, &Settings::onUrlEdited,
                qontrol::UNIQUE);
        connect(serverRow.field->testButton(), &QPushButton::clicked, this,
                &Settings::onTestClicked, qontrol::UNIQUE);
        connect(serverRow.connect, &QPushButton::clicked, this, &Settings::onConnectClicked,
                qontrol::UNIQUE);
        connect(serverRow.disconnect, &QPushButton::clicked, this, &Settings::onDisconnectClicked,
                qontrol::UNIQUE);
        if (serverRow.ssl != nullptr) {
            connect(serverRow.ssl, &QCheckBox::clicked, this, &Settings::onSslToggled,
                    qontrol::UNIQUE);
            connect(serverRow.verify, &QCheckBox::clicked, this, &Settings::onVerifyToggled,
                    qontrol::UNIQUE);
        }
    }
    connect(m_height_timer, &QTimer::timeout, this, &Settings::onHeightTimeout, qontrol::UNIQUE);
    connect(m_backup, &QPushButton::clicked, this, &Settings::onBackupClicked, qontrol::UNIQUE);
    // A provider without the extension answers error(): its operations are never busy here.
    m_busy_req = nextRequestId();
    m_services.settings->value(m_busy_req, extension::ENDPOINTS_BUSY, QJsonValue());
}

void Settings::connectServices() const {
    auto *settings = m_services.settings;
    auto *blockchain = m_services.blockchain;
    connect(settings, &backend::IBackendSettings::settingsChanged, this,
            &Settings::onEndpointsChanged, qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::stateChanged, this, &Settings::onEndpointsChanged,
            qontrol::UNIQUE);
    connect(settings, &backend::IBackendSettings::endpointTested, this, &Settings::onEndpointTested,
            qontrol::UNIQUE);
    connect(settings, &backend::IBackendSettings::saved, this, &Settings::onSettingsSaved,
            qontrol::UNIQUE);
    connect(settings, &backend::IService::valueResult, this, &Settings::onSettingsValue,
            qontrol::UNIQUE);
    connect(settings, &backend::IService::valueEvent, this, &Settings::onSettingsEvent,
            qontrol::UNIQUE);
    connect(settings, &backend::IService::error, this, &Settings::onServiceError, qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::completed, this, &Settings::onBlockchainCompleted,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IService::error, this, &Settings::onServiceError,
            qontrol::UNIQUE);
}

void Settings::view() {
    auto *network = catalog::pillNetwork(networkName(m_services.settings->settings().network));
    auto *col = (new qontrol::Column)->spacing(resolve(Spacing::L));
    for (const auto &serverRow : m_rows) {
        auto *server = viewRow(serverRow);
        if (serverRow.presentation.capabilities) {
            // What the server reports it serves.
            auto *capabilities = new catalog::Field(TR("settings-capabilities"), m_capabilities);
            capabilities->setTopAligned(true);
            server = (new qontrol::Column)
                         ->spacing(resolve(Spacing::S))
                         ->push(server)
                         ->push(capabilities);
        }
        col->push(server);
    }
    if (m_extras != nullptr) {
        col->push(m_extras);
    }
    auto *backupRow = (new qontrol::Row)->pushSpacer()->push(m_backup);
    auto *backup = new catalog::Section(TR("settings-backup"));
    backup->setContent(
        (new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_backup_text)->push(backupRow));
    col->push(backup);
    col->pushSpacer();
    setScreenContent(this, m_main_widget, dashboard(TR("settings-title"), col, false, network));
}

void Settings::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    qontrol::Screen::changeEvent(event);
}

void Settings::showEvent(QShowEvent *event) {
    for (auto &serverRow : m_rows) {
        requestHeight(serverRow);
    }
    m_height_timer->start();
    qontrol::Screen::showEvent(event);
}

void Settings::hideEvent(QHideEvent *event) {
    m_height_timer->stop();
    qontrol::Screen::hideEvent(event);
}

// The provider says the texts in the language now in use.
void Settings::retranslateUi() {
    m_backup_text->setText(TR("settings-backup-text"));
    m_presentation_req = nextRequestId();
    m_services.settings->value(m_presentation_req, extension::ENDPOINT_PRESENTATION, QJsonValue());
    showPresentation();
}

void Settings::showPresentation() {
    for (auto &serverRow : m_rows) {
        auto presentation = presentationOf(serverRow.endpoint);
        serverRow.presentation.label = presentation.label;
        serverRow.presentation.placeholder = presentation.placeholder;
        serverRow.presentation.connect = presentation.connect;
        serverRow.presentation.disconnect = presentation.disconnect;
        serverRow.label->setText(presentation.label);
        serverRow.field->input()->setPlaceholderText(presentation.placeholder);
        serverRow.connect->setText(presentation.connect);
        serverRow.disconnect->setText(presentation.disconnect);
    }
}

auto Settings::viewRow(const ServerRow &row) -> QWidget * {
    auto *server = (new qontrol::Column)
                       ->spacing(resolve(Spacing::XS))
                       ->push(row.label)
                       ->push(row.field->control())
                       ->push(row.field->note());
    if (row.ssl == nullptr) {
        return server;
    }
    auto *verify = (new qontrol::Row)
                       ->spacing(resolve(Spacing::XS))
                       ->push(new catalog::InfoTip(TR("musig-electrum-verify-tip")))
                       ->push(row.verify);
    auto *sslField = new catalog::Field(TR("musig-electrum-ssl"), row.ssl);
    auto *verifyField = new catalog::Field(TR("musig-electrum-verify-certificate"), verify);
    // Each field as wide as its label and toggle, the spacer takes the rest.
    sslField->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    verifyField->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto *transport = (new qontrol::Row)
                          ->spacing(resolve(Spacing::L))
                          ->push(sslField)
                          ->push(verifyField)
                          ->pushSpacer();
    return (new qontrol::Column)->spacing(resolve(Spacing::S))->push(server)->push(transport);
}

auto Settings::rowOf(const QObject *widget) -> ServerRow * {
    for (auto &serverRow : m_rows) {
        if (serverRow.field->input() == widget || serverRow.field->testButton() == widget ||
            serverRow.connect == widget || serverRow.disconnect == widget ||
            serverRow.ssl == widget || serverRow.verify == widget) {
            return &serverRow;
        }
    }
    return nullptr;
}

auto Settings::row(const QString &id) -> ServerRow * {
    for (auto &serverRow : m_rows) {
        if (serverRow.endpoint.id == id) {
            return &serverRow;
        }
    }
    return nullptr;
}

auto Settings::rowOfRequest(backend::RequestId request_id) -> ServerRow * {
    for (auto &serverRow : m_rows) {
        if (request_id != 0 &&
            (serverRow.action_req == request_id || serverRow.tip_req == request_id)) {
            return &serverRow;
        }
    }
    return nullptr;
}

// What the provider says of `endpoint`, its name alone when it says nothing.
auto Settings::presentationOf(const backend::Endpoint &endpoint) const -> Presentation {
    auto known = m_presentation.value(endpoint.id).toObject();
    auto text = [&known, &endpoint](const char *key) {
        auto value = known.value(QLatin1String(key)).toString();
        return value.isEmpty() ? endpoint.name : value;
    };
    return {.label = text("label"),
            .placeholder = known.value(QStringLiteral("placeholder")).toString(),
            .connect = text("connect"),
            .disconnect = text("disconnect"),
            .transport = known.value(QStringLiteral("transport")).toBool(),
            .capabilities = known.value(QStringLiteral("capabilities")).toBool()};
}

auto Settings::connection(const QString &id) const -> std::optional<backend::Connection> {
    for (const auto &connection : m_services.blockchain->state().connections) {
        if (connection.id == id) {
            return connection;
        }
    }
    return std::nullopt;
}

auto Settings::isRunning(const ServerRow &row) const -> bool {
    auto connection = this->connection(row.endpoint.id);
    return connection.has_value() && connection->connected;
}

auto Settings::candidateUrl(const ServerRow &row) -> QString {
    auto address = row.field->input()->text().trimmed();
    if (row.ssl != nullptr && row.ssl->isChecked() && !address.isEmpty() &&
        !address.contains(K_SCHEME_SEPARATOR)) {
        return K_SSL_SCHEME + address;
    }
    return address;
}

auto Settings::candidateVerify(const ServerRow &row) -> bool {
    // Only an SSL server presents a certificate to check.
    return row.ssl == nullptr || !row.ssl->isChecked() || row.verify_wanted;
}

auto Settings::isDirty(const ServerRow &row) -> bool {
    auto url = candidateUrl(row);
    if (url.isEmpty()) {
        return false;
    }
    // A tested plaintext server is saved without its scheme, one saved by hand may have it.
    auto saved = row.saved_url;
    if (url.startsWith(K_TCP_SCHEME)) {
        url = url.mid(K_TCP_SCHEME.size());
    }
    if (saved.startsWith(K_TCP_SCHEME)) {
        saved = saved.mid(K_TCP_SCHEME.size());
    }
    return url != saved || candidateVerify(row) != row.saved_verify;
}

auto Settings::isBusy() const -> bool {
    return m_endpoints_busy || std::ranges::any_of(m_rows, [](const ServerRow &server_row) {
               return server_row.action_req != 0;
           });
}

void Settings::loadSaved(ServerRow &row, const backend::Endpoint &saved) {
    row.endpoint = saved;
    row.saved_url = saved.url;
    row.saved_verify =
        row.ssl == nullptr || !row.saved_url.startsWith(K_SSL_SCHEME) || saved.verifyCertificate;
}

void Settings::showSaved(ServerRow &row) {
    row.field->input()->setText(row.saved_url);
    if (row.ssl != nullptr) {
        row.ssl->setChecked(row.saved_url.startsWith(K_SSL_SCHEME));
        row.verify_wanted = row.saved_verify;
    }
}

void Settings::showError(const ServerRow &row, const QString &error) {
    row.field->setError(error);
}

void Settings::refreshRows() {
    for (const auto &serverRow : m_rows) {
        refreshRow(serverRow);
    }
}

void Settings::refreshRow(const ServerRow &row) {
    bool busy = isBusy();
    bool dirty = isDirty(row);
    bool running = isRunning(row);
    // The lock holds a connected server reached over a transport (Electrum), the one a
    // Multikey account syncs from.
    bool addressLocked = m_lock_connected && row.presentation.transport && running;

    row.field->setAvailable(!busy, dirty);
    row.field->input()->setReadOnly(addressLocked);

    row.connect->setVisible(!running);
    row.disconnect->setVisible(running);
    row.connect->setEnabled(!busy && !dirty && !row.saved_url.isEmpty());
    row.disconnect->setEnabled(!busy);

    if (row.ssl != nullptr) {
        bool ssl = row.ssl->isChecked();
        row.ssl->setEnabled(!busy && !addressLocked);
        // The shown state follows SSL, the choice stays in verify_wanted.
        row.verify->setChecked(ssl && row.verify_wanted);
        row.verify->setEnabled(!busy && ssl);
    }
}

void Settings::requestHeight(ServerRow &row) {
    if (m_endpoints_busy || !isRunning(row)) {
        row.tip_req = 0;
        row.height->clear();
        return;
    }
    row.tip_req = nextRequestId();
    m_services.blockchain->refreshTip(row.tip_req, row.endpoint.id);
}

void Settings::requestCapabilities() {
    if (m_info_req != 0 || m_endpoints_busy) {
        return;
    }
    auto found = std::ranges::find_if(
        m_rows, [](const ServerRow &server_row) { return server_row.presentation.capabilities; });
    if (found == m_rows.end()) {
        return;
    }
    m_capabilities->setText("--");
    if (found->saved_url.isEmpty()) {
        return;
    }
    m_info_req = nextRequestId();
    m_services.settings->value(m_info_req, extension::ENDPOINT_CAPABILITIES, found->endpoint.id);
}

void Settings::startAction(ServerRow &row, Step step) {
    row.action_req = nextRequestId();
    row.step = step;
    row.action_revision = row.draft_revision;
    row.saved = false;
    if (step == Step::Testing) {
        row.tested = row.endpoint;
        row.tested.url = candidateUrl(row);
        row.tested.verifyCertificate = candidateVerify(row);
        row.field->setState(catalog::EndpointRow::State::Testing);
    }
    showError(row, QString());
    refreshRows();
}

void Settings::finishTest(ServerRow &row, const QString &error) {
    bool current = row.action_revision == row.draft_revision;
    bool saved = std::exchange(row.saved, false);
    row.action_req = 0;
    row.step = Step::None;
    showError(row, QString());
    for (const auto &endpoint : m_services.settings->settings().endpoints) {
        if (endpoint.id == row.endpoint.id) {
            loadSaved(row, endpoint);
        }
    }
    if (saved) {
        if (current) {
            showSaved(row);
        }
        if (row.presentation.capabilities) {
            requestCapabilities();
        }
    }
    if (current) {
        row.field->setState(error.isEmpty() && saved ? catalog::EndpointRow::State::Ok
                                                     : catalog::EndpointRow::State::Failed,
                            error);
    }
    refreshRows();
    if (isVisible()) {
        requestHeight(row);
    }
}

// ===== Slots =====

void Settings::onTestClicked() {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr || isBusy() || !isDirty(*serverRow)) {
        return;
    }
    startAction(*serverRow, Step::Testing);
    m_services.settings->testEndpoint(serverRow->action_req, serverRow->tested);
}

void Settings::onConnectClicked() {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr || isBusy()) {
        return;
    }
    startAction(*serverRow, Step::Connecting);
    m_services.blockchain->setConnected(serverRow->action_req, serverRow->endpoint.id, true);
}

void Settings::onDisconnectClicked() {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr || isBusy()) {
        return;
    }
    startAction(*serverRow, Step::Disconnecting);
    m_services.blockchain->setConnected(serverRow->action_req, serverRow->endpoint.id, false);
}

void Settings::onUrlEdited() {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr) {
        return;
    }
    ++serverRow->draft_revision;
    serverRow->field->setState(catalog::EndpointRow::State::Untested);
    if (serverRow->ssl != nullptr) {
        auto url = serverRow->field->input()->text().trimmed();
        if (url.startsWith(K_SSL_SCHEME)) {
            serverRow->ssl->setChecked(true);
        } else if (url.startsWith(K_TCP_SCHEME)) {
            serverRow->ssl->setChecked(false);
        }
    }
    showError(*serverRow, QString());
    refreshRow(*serverRow);
}

void Settings::onSslToggled(bool checked) {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr) {
        return;
    }
    // Another scheme: the server is tested again under it.
    ++serverRow->draft_revision;
    serverRow->field->setState(catalog::EndpointRow::State::Untested);
    auto bare = serverRow->field->input()->text().trimmed();
    if (bare.contains(K_SCHEME_SEPARATOR)) {
        bare = bare.section(K_SCHEME_SEPARATOR, 1);
    }
    if (!bare.isEmpty()) {
        serverRow->field->input()->setText((checked ? K_SSL_SCHEME : K_TCP_SCHEME) + bare);
    }
    showError(*serverRow, QString());
    refreshRow(*serverRow);
}

void Settings::onVerifyToggled(bool checked) {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr) {
        return;
    }
    serverRow->verify_wanted = checked;
    ++serverRow->draft_revision;
    serverRow->field->setState(catalog::EndpointRow::State::Untested);
    showError(*serverRow, QString());
    refreshRow(*serverRow);
}

// The saved servers or their connections changed.
void Settings::onEndpointsChanged() {
    refreshEndpoints(false);
}

// Heights and capabilities are asked again only when a server's saved address, its
// connection or the busy state changed: not on every scan or tip update.
void Settings::refreshEndpoints(bool busy_changed) {
    auto endpoints = m_services.settings->settings().endpoints;
    bool capabilitiesChanged = busy_changed;
    for (auto &serverRow : m_rows) {
        auto savedUrl = serverRow.saved_url;
        bool savedVerify = serverRow.saved_verify;
        auto saved = std::ranges::find_if(endpoints, [&serverRow](const backend::Endpoint &e) {
            return e.id == serverRow.endpoint.id;
        });
        if (saved != endpoints.end()) {
            bool dirty = isDirty(serverRow);
            loadSaved(serverRow, *saved);
            if (!dirty && serverRow.action_req == 0) {
                if (savedUrl != serverRow.saved_url || savedVerify != serverRow.saved_verify) {
                    ++serverRow.draft_revision;
                    serverRow.field->setState(catalog::EndpointRow::State::Untested);
                }
                showSaved(serverRow);
            }
        }
        auto connection = this->connection(serverRow.endpoint.id);
        bool connected = connection.has_value() && connection->connected;
        auto error = connection.has_value() ? connection->error : QString();
        bool changed = busy_changed || connected != serverRow.seen_connected ||
                       error != serverRow.seen_error || savedUrl != serverRow.saved_url ||
                       savedVerify != serverRow.saved_verify;
        serverRow.seen_connected = connected;
        serverRow.seen_error = error;
        if (!error.isEmpty() && serverRow.step != Step::Testing) {
            showError(serverRow, error);
        } else if (serverRow.step == Step::None) {
            showError(serverRow, QString());
        }
        if (!changed) {
            continue;
        }
        capabilitiesChanged = capabilitiesChanged || serverRow.presentation.capabilities;
        if (m_endpoints_busy) {
            serverRow.tip_req = 0;
            serverRow.height->clear();
        } else if (isVisible() && serverRow.tip_req == 0) {
            requestHeight(serverRow);
        }
    }
    if (capabilitiesChanged) {
        requestCapabilities();
    }
    refreshRows();
}

void Settings::onEndpointTested(backend::RequestId request_id) {
    auto *serverRow = rowOfRequest(request_id);
    if (serverRow == nullptr || serverRow->step != Step::Testing) {
        return;
    }
    // Passed: saved untested, as the tested server is what Test saves.
    serverRow->action_req = nextRequestId();
    serverRow->step = Step::Saving;
    auto settings = m_services.settings->settings();
    settings.endpoints = {serverRow->tested};
    m_services.settings->save(serverRow->action_req, settings);
}

void Settings::onSettingsSaved(backend::RequestId request_id) {
    auto *serverRow = rowOfRequest(request_id);
    if (serverRow == nullptr || serverRow->step != Step::Saving) {
        return;
    }
    // Saved: the tested server is left disconnected, so the account reopens on it.
    serverRow->saved = true;
    serverRow->action_req = nextRequestId();
    serverRow->step = Step::Disconnecting;
    m_services.blockchain->setConnected(serverRow->action_req, serverRow->endpoint.id, false);
}

void Settings::onBlockchainCompleted(backend::RequestId request_id) {
    auto *serverRow = rowOfRequest(request_id);
    if (serverRow == nullptr) {
        return;
    }
    if (request_id == serverRow->tip_req) {
        serverRow->tip_req = 0;
        auto connection = this->connection(serverRow->endpoint.id);
        if (connection.has_value() && connection->tip.has_value()) {
            serverRow->height->setText(TR("settings-height").arg(*connection->tip));
        } else {
            serverRow->height->clear();
        }
        return;
    }
    if (serverRow->saved) {
        finishTest(*serverRow, QString());
        return;
    }
    serverRow->action_req = 0;
    serverRow->step = Step::None;
    refreshRows();
    if (isVisible()) {
        requestHeight(*serverRow);
    }
}

void Settings::onServiceError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0) {
        return;
    }
    if (request_id == m_busy_req) {
        m_busy_req = 0;
        return;
    }
    if (request_id == m_presentation_req) {
        m_presentation_req = 0;
        return;
    }
    if (request_id == m_info_req) {
        m_info_req = 0;
        m_capabilities->setText("--");
        return;
    }
    auto *serverRow = rowOfRequest(request_id);
    if (serverRow == nullptr) {
        return;
    }
    if (request_id == serverRow->tip_req) {
        serverRow->tip_req = 0;
        serverRow->height->clear();
        return;
    }
    if (serverRow->step == Step::Testing || serverRow->step == Step::Saving || serverRow->saved) {
        finishTest(*serverRow, message);
        return;
    }
    serverRow->action_req = 0;
    serverRow->step = Step::None;
    showError(*serverRow, message);
    refreshRows();
    if (isVisible()) {
        requestHeight(*serverRow);
    }
}

void Settings::onSettingsValue(backend::RequestId request_id, const QJsonValue &payload) {
    if (request_id != 0 && request_id == m_presentation_req) {
        m_presentation_req = 0;
        m_presentation = payload.toObject();
        showPresentation();
        return;
    }
    if (request_id != 0 && request_id == m_busy_req) {
        m_busy_req = 0;
        onSettingsEvent(extension::ENDPOINTS_BUSY, payload);
        return;
    }
    if (request_id == 0 || request_id != m_info_req) {
        return;
    }
    m_info_req = 0;
    auto info = payload.toObject();
    auto yesText = TR("common-yes");
    auto noText = TR("common-no");
    auto yn = [&yesText, &noText, &info](const char *key) -> QString {
        return info.value(QLatin1String(key)).toBool() ? yesText : noText;
    };
    m_capabilities->setText(TR("settings-capabilities-template")
                                .arg(yn("tweaksOnly"))
                                .arg(yn("tweaksFullBasic"))
                                .arg(yn("tweaksFullWithDustFilter"))
                                .arg(yn("tweaksCutThroughWithDustFilter")));
}

void Settings::onSettingsEvent(const QString &operation, const QJsonValue &payload) {
    if (operation != extension::ENDPOINTS_BUSY || payload.toBool() == m_endpoints_busy) {
        return;
    }
    m_endpoints_busy = payload.toBool();
    if (m_endpoints_busy) {
        m_info_req = 0;
    }
    refreshEndpoints(true);
}

void Settings::onHeightTimeout() {
    for (auto &serverRow : m_rows) {
        requestHeight(serverRow);
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const)
void Settings::onBackupClicked() {
    qontrol::Controller::execModal(new BackupAccount(m_services.account));
}

} // namespace account::view
