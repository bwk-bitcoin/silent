#include "account/views/Settings.h"

#include "account/RequestId.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Badge.h"
#include "catalog/display/InfoTip.h"
#include "catalog/display/Label.h"
#include "catalog/form/EndpointRow.h"
#include "catalog/form/Field.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/Toggle.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QSizePolicy>
#include <QTimer>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <string>

namespace {

// How often the server heights are asked while Settings shows.
constexpr int HEIGHT_REFRESH_MS = 30000;

const QString K_SSL_SCHEME = QStringLiteral("ssl://");
const QString K_TCP_SCHEME = QStringLiteral("tcp://");
const QString K_SCHEME_SEPARATOR = QStringLiteral("://");

auto toQString(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

} // namespace

namespace account::view {

Settings::Settings(AccountBackend *backend, bool lock_connected_electrum)
    : m_backend(backend),
      m_lock_connected_electrum(lock_connected_electrum) {
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
    for (auto endpoint : m_backend->endpoints()) {
        ServerRow serverRow;
        serverRow.endpoint = endpoint;
        serverRow.label = new catalog::Label(catalog::LabelRole::Eyebrow);
        serverRow.label->setTone(catalog::LabelTone::Muted);
        serverRow.height = new catalog::Label(catalog::LabelRole::Mono);
        serverRow.height->setTone(catalog::LabelTone::Secondary);
        // A fixed slot, so stacked rows' inputs share one width.
        serverRow.height->setFixedWidth(resolve(Size::XS));
        bool blindbit = endpoint == Endpoint::Blindbit;
        serverRow.connect =
            blindbit ? catalog::btnConnectBlindbit() : catalog::btnConnectElectrum();
        serverRow.disconnect =
            blindbit ? catalog::btnDisconnectBlindbit() : catalog::btnDisconnectElectrum();
        auto *controls = (new qontrol::Row)
                             ->spacing(resolve(Spacing::S))
                             ->push(serverRow.height)
                             ->push(serverRow.connect)
                             ->push(serverRow.disconnect);
        serverRow.field = new catalog::EndpointRow(controls);
        if (!blindbit) {
            serverRow.ssl = new catalog::Toggle;
            serverRow.verify = new catalog::Toggle;
        }
        loadSaved(serverRow);
        showSaved(serverRow);
        auto error = m_backend->endpointError(endpoint);
        if (!error.isEmpty()) {
            showError(serverRow, mapBackendErrorSummary(m_backend->endpointErrorKind(endpoint)) +
                                     QStringLiteral(" ") + error);
        }
        m_rows.append(serverRow);
    }

    m_capabilities = new catalog::Label("--");
    m_capabilities->setAlignment(Qt::AlignTop);
    m_height_timer = new QTimer(this);
    m_height_timer->setInterval(HEIGHT_REFRESH_MS);
    m_extras = m_backend->settingsExtras();

    retranslateUi();
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
    connect(m_backend, &AccountBackend::endpointStateChanged, this,
            &Settings::onEndpointStateChanged, qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::endpointRequestFinished, this,
            &Settings::onEndpointRequestFinished, qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::endpointTipReady, this, &Settings::onEndpointTipReady,
            qontrol::UNIQUE);
    connect(m_backend, &AccountBackend::endpointInfoReady, this, &Settings::onBackendInfoReady,
            qontrol::UNIQUE);
    connect(m_height_timer, &QTimer::timeout, this, &Settings::onHeightTimeout, qontrol::UNIQUE);
}

void Settings::view() {
    auto *network = catalog::pillNetwork(networkName(m_backend->network()));
    auto *col = (new qontrol::Column)->spacing(resolve(Spacing::L));
    for (const auto &serverRow : m_rows) {
        auto *server = viewRow(serverRow);
        if (serverRow.endpoint == Endpoint::Blindbit) {
            // What the BlindBit server reports it serves.
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

void Settings::retranslateUi() {
    for (const auto &serverRow : m_rows) {
        serverRow.label->setText(label(serverRow.endpoint));
        serverRow.field->input()->setPlaceholderText(serverRow.endpoint == Endpoint::Blindbit
                                                         ? TR("settings-placeholder-blindbit")
                                                         : TR("settings-placeholder-electrum"));
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

auto Settings::row(Endpoint endpoint) -> ServerRow * {
    for (auto &serverRow : m_rows) {
        if (serverRow.endpoint == endpoint) {
            return &serverRow;
        }
    }
    return nullptr;
}

auto Settings::label(Endpoint endpoint) -> QString {
    return endpoint == Endpoint::Blindbit ? TR("settings-blindbit-address")
                                          : TR("settings-electrum-address");
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
    return m_backend->endpointOperationsBusy() ||
           std::ranges::any_of(
               m_rows, [](const ServerRow &server_row) { return server_row.action_req != 0; });
}

void Settings::loadSaved(ServerRow &row) {
    row.saved_url = m_backend->endpointUrl(row.endpoint);
    row.saved_verify = row.ssl == nullptr || !row.saved_url.startsWith(K_SSL_SCHEME) ||
                       m_backend->verifiesCertificate();
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
    bool running = m_backend->isEndpointRunning(row.endpoint);
    bool addressLocked = m_lock_connected_electrum && row.endpoint == Endpoint::Electrum && running;

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
    if (m_backend->endpointOperationsBusy() || !m_backend->isEndpointRunning(row.endpoint)) {
        row.tip_req = 0;
        row.height->clear();
        return;
    }
    row.tip_req = nextRequestId();
    m_backend->requestEndpointTip(row.tip_req, row.endpoint);
}

void Settings::requestCapabilities() {
    if (m_info_req != 0 || m_backend->endpointOperationsBusy()) {
        return;
    }
    auto *blindbit = row(Endpoint::Blindbit);
    if (blindbit == nullptr) {
        return;
    }
    m_capabilities->setText("--");
    if (blindbit->saved_url.isEmpty()) {
        m_info_req = 0;
        return;
    }
    m_info_req = nextRequestId();
    m_backend->requestEndpointInfo(m_info_req, Endpoint::Blindbit);
}

void Settings::startAction(ServerRow &row, bool test) {
    row.action_req = nextRequestId();
    row.testing_req = test;
    row.action_revision = row.draft_revision;
    if (test) {
        row.field->setState(catalog::EndpointRow::State::Testing);
    }
    showError(row, QString());
    refreshRows();
}

// ===== Slots =====

void Settings::onTestClicked() {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr || isBusy() || !isDirty(*serverRow)) {
        return;
    }
    startAction(*serverRow, true);
    m_backend->testAndSaveEndpoint(serverRow->action_req, serverRow->endpoint,
                                   candidateUrl(*serverRow), candidateVerify(*serverRow));
}

void Settings::onConnectClicked() {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr || isBusy()) {
        return;
    }
    startAction(*serverRow, false);
    m_backend->setEndpointConnected(serverRow->action_req, serverRow->endpoint, true);
}

void Settings::onDisconnectClicked() {
    auto *serverRow = rowOf(sender());
    if (serverRow == nullptr || isBusy()) {
        return;
    }
    startAction(*serverRow, false);
    m_backend->setEndpointConnected(serverRow->action_req, serverRow->endpoint, false);
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

void Settings::onEndpointStateChanged(Endpoint endpoint) {
    auto *serverRow = row(endpoint);
    if (serverRow != nullptr) {
        bool dirty = isDirty(*serverRow);
        auto savedUrl = serverRow->saved_url;
        bool savedVerify = serverRow->saved_verify;
        loadSaved(*serverRow);
        if (!dirty && serverRow->action_req == 0) {
            if (savedUrl != serverRow->saved_url || savedVerify != serverRow->saved_verify) {
                ++serverRow->draft_revision;
                serverRow->field->setState(catalog::EndpointRow::State::Untested);
            }
            showSaved(*serverRow);
        }
        auto error = m_backend->endpointError(endpoint);
        if (!error.isEmpty() && !serverRow->testing_req) {
            showError(*serverRow, mapBackendErrorSummary(m_backend->endpointErrorKind(endpoint)) +
                                      QStringLiteral(" ") + error);
        } else {
            showError(*serverRow, QString());
        }
        if (m_backend->endpointOperationsBusy()) {
            serverRow->tip_req = 0;
            serverRow->height->clear();
            m_info_req = 0;
        } else if (isVisible()) {
            requestHeight(*serverRow);
        }
        if (endpoint == Endpoint::Blindbit) {
            requestCapabilities();
        }
    }
    refreshRows();
}

void Settings::onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                         const EndpointResult &result) {
    auto *serverRow = row(endpoint);
    if (serverRow == nullptr || req_id == 0 || serverRow->action_req != req_id) {
        return;
    }
    bool tested = serverRow->testing_req;
    bool current = serverRow->action_revision == serverRow->draft_revision;
    serverRow->action_req = 0;
    serverRow->testing_req = false;
    loadSaved(*serverRow);
    if (tested) {
        showError(*serverRow, QString());
    }
    if (result.saved) {
        if (current) {
            showSaved(*serverRow);
        }
        if (endpoint == Endpoint::Blindbit) {
            requestCapabilities();
        }
    }
    if (tested && current) {
        serverRow->field->setState(
            result.is_ok && result.saved ? catalog::EndpointRow::State::Ok
                                         : catalog::EndpointRow::State::Failed,
            result.is_ok ? QString()
                         : mapBackendErrorSummary(result.kind) + QStringLiteral(" ") +
                               toQString(result.error));
    } else if (!tested && !result.is_ok) {
        showError(*serverRow, mapBackendErrorSummary(result.kind) + QStringLiteral(" ") +
                                  toQString(result.error));
    }
    refreshRows();
    if (isVisible()) {
        requestHeight(*serverRow);
    }
}

void Settings::onEndpointTipReady(uint64_t req_id, Endpoint endpoint,
                                  const ElectrumTipResult &result) {
    auto *serverRow = row(endpoint);
    if (serverRow == nullptr || req_id == 0 || serverRow->tip_req != req_id) {
        return;
    }
    serverRow->tip_req = 0;
    if (!result.is_ok) {
        serverRow->height->clear();
        return;
    }
    serverRow->height->setText(TR("settings-height").arg(result.height));
}

void Settings::onBackendInfoReady(uint64_t req_id, Endpoint endpoint, const BackendInfo &info) {
    if (endpoint != Endpoint::Blindbit || req_id == 0 || req_id != m_info_req) {
        return;
    }
    m_info_req = 0;
    if (!info.is_ok) {
        m_capabilities->setText("--");
        return;
    }
    auto yesText = TR("common-yes");
    auto noText = TR("common-no");
    auto yn = [&yesText, &noText](bool value) -> QString { return value ? yesText : noText; };
    m_capabilities->setText(TR("settings-capabilities-template")
                                .arg(yn(info.tweaks_only))
                                .arg(yn(info.tweaks_full_basic))
                                .arg(yn(info.tweaks_full_with_dust_filter))
                                .arg(yn(info.tweaks_cut_through_with_dust_filter)));
}

void Settings::onHeightTimeout() {
    for (auto &serverRow : m_rows) {
        requestHeight(serverRow);
    }
}

} // namespace account::view
