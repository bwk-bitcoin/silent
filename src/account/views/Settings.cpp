#include "account/views/Settings.h"

#include "account/RequestId.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Badge.h"
#include "catalog/display/InfoTip.h"
#include "catalog/display/Label.h"
#include "catalog/form/Field.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/Toggle.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QSizePolicy>
#include <QThread>
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

Settings::Settings(AccountBackend *backend) : m_backend(backend) {
    init();
    doConnect();
    view();
    requestCapabilities();
}

void Settings::init() {
    for (auto endpoint : m_backend->endpoints()) {
        ServerRow serverRow;
        serverRow.endpoint = endpoint;
        serverRow.label = new catalog::Label(catalog::LabelRole::Eyebrow);
        serverRow.label->setTone(catalog::LabelTone::Muted);
        serverRow.url = new catalog::Input;
        serverRow.height = new catalog::Label(catalog::LabelRole::Mono);
        serverRow.height->setTone(catalog::LabelTone::Secondary);
        // A fixed slot, so stacked rows' inputs share one width.
        serverRow.height->setFixedWidth(resolve(Size::XS));
        serverRow.test = catalog::btnTest();
        serverRow.testing = catalog::btnTesting();
        serverRow.testing->setEnabled(false);
        bool blindbit = endpoint == Endpoint::Blindbit;
        serverRow.connect =
            blindbit ? catalog::btnConnectBlindbit() : catalog::btnConnectElectrum();
        serverRow.disconnect =
            blindbit ? catalog::btnDisconnectBlindbit() : catalog::btnDisconnectElectrum();
        serverRow.note = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Error);
        serverRow.note->setVisible(false);
        if (!blindbit) {
            serverRow.ssl = new catalog::Toggle;
            serverRow.verify = new catalog::Toggle;
        }
        loadSaved(serverRow);
        showSaved(serverRow);
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
        connect(serverRow.url, &QLineEdit::textEdited, this, &Settings::onUrlEdited,
                qontrol::UNIQUE);
        connect(serverRow.test, &QPushButton::clicked, this, &Settings::onTestClicked,
                qontrol::UNIQUE);
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
        serverRow.url->setPlaceholderText(serverRow.endpoint == Endpoint::Blindbit
                                              ? TR("settings-placeholder-blindbit")
                                              : TR("settings-placeholder-electrum"));
    }
}

auto Settings::viewRow(const ServerRow &row) -> QWidget * {
    auto *control = (new qontrol::Row)
                        ->spacing(resolve(Spacing::S))
                        ->push(row.url, 1)
                        ->push(row.height)
                        ->push(row.test)
                        ->push(row.testing)
                        ->push(row.connect)
                        ->push(row.disconnect);
    auto *server = (new qontrol::Column)
                       ->spacing(resolve(Spacing::XS))
                       ->push(row.label)
                       ->push(control)
                       ->push(row.note);
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
        if (serverRow.url == widget || serverRow.test == widget || serverRow.connect == widget ||
            serverRow.disconnect == widget || serverRow.ssl == widget ||
            serverRow.verify == widget) {
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
    auto address = row.url->text().trimmed();
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
    return std::ranges::any_of(
        m_rows, [](const ServerRow &server_row) { return server_row.action_req != 0; });
}

void Settings::loadSaved(ServerRow &row) {
    row.saved_url = m_backend->endpointUrl(row.endpoint);
    row.saved_verify = row.ssl == nullptr || !row.saved_url.startsWith(K_SSL_SCHEME) ||
                       m_backend->verifiesCertificate();
}

void Settings::showSaved(ServerRow &row) {
    row.url->setText(row.saved_url);
    if (row.ssl != nullptr) {
        row.ssl->setChecked(row.saved_url.startsWith(K_SSL_SCHEME));
        row.verify_wanted = row.saved_verify;
    }
}

void Settings::showError(const ServerRow &row, const QString &error) {
    row.note->setText(error);
    row.note->setVisible(!error.isEmpty());
}

void Settings::refreshRows() {
    for (const auto &serverRow : m_rows) {
        refreshRow(serverRow);
    }
}

void Settings::refreshRow(const ServerRow &row) {
    bool busy = isBusy();
    bool testing = row.action_req != 0 && row.testing_req;
    bool dirty = isDirty(row);
    bool running = m_backend->isEndpointRunning(row.endpoint);

    row.url->setEnabled(!testing);
    // Test keeps its slot while hidden, Testing… takes it while the test runs.
    auto policy = row.test->sizePolicy();
    policy.setRetainSizeWhenHidden(!testing);
    row.test->setSizePolicy(policy);
    row.test->setVisible(!testing && dirty);
    row.test->setEnabled(!busy);
    row.testing->setVisible(testing);

    row.connect->setVisible(!running);
    row.disconnect->setVisible(running);
    row.connect->setEnabled(!busy && !dirty && !row.saved_url.isEmpty());
    row.disconnect->setEnabled(!busy);

    if (row.ssl != nullptr) {
        bool ssl = row.ssl->isChecked();
        row.ssl->setEnabled(!testing);
        // The shown state follows SSL, the choice stays in verify_wanted.
        row.verify->setChecked(ssl && row.verify_wanted);
        row.verify->setEnabled(!testing && ssl);
    }
}

void Settings::requestHeight(ServerRow &row) {
    if (!m_backend->isEndpointRunning(row.endpoint)) {
        row.tip_req = 0;
        row.height->clear();
        return;
    }
    row.tip_req = nextRequestId();
    m_backend->requestEndpointTip(row.tip_req, row.endpoint);
}

void Settings::requestCapabilities() {
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
    // The reply outlives the request even if the Settings view closes during the fetch.
    auto *reply = new BackendInfoReply;
    auto *worker =
        QThread::create([reply, req = m_info_req, url = blindbit->saved_url.toStdString()]() {
            emit reply->ready(req, ::get_backend_info(rust::String(url)));
        });
    connect(reply, &BackendInfoReply::ready, this, &Settings::onBackendInfoReady, qontrol::UNIQUE);
    connect(worker, &QThread::finished, reply, &QObject::deleteLater, qontrol::UNIQUE);
    m_backend->startWorker(worker);
}

void Settings::startAction(ServerRow &row, bool test) {
    row.action_req = nextRequestId();
    row.testing_req = test;
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
    if (serverRow->ssl != nullptr) {
        auto url = serverRow->url->text().trimmed();
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
    auto bare = serverRow->url->text().trimmed();
    if (bare.contains(K_SCHEME_SEPARATOR)) {
        bare = bare.section(K_SCHEME_SEPARATOR, 1);
    }
    if (!bare.isEmpty()) {
        serverRow->url->setText((checked ? K_SSL_SCHEME : K_TCP_SCHEME) + bare);
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
    showError(*serverRow, QString());
    refreshRow(*serverRow);
}

void Settings::onEndpointStateChanged(Endpoint endpoint) {
    refreshRows();
    auto *serverRow = row(endpoint);
    if (serverRow != nullptr && isVisible()) {
        requestHeight(*serverRow);
    }
}

void Settings::onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                         const EndpointResult &result) {
    auto *serverRow = row(endpoint);
    if (serverRow == nullptr || req_id == 0 || serverRow->action_req != req_id) {
        return;
    }
    serverRow->action_req = 0;
    serverRow->testing_req = false;
    loadSaved(*serverRow);
    if (result.saved) {
        showSaved(*serverRow);
        if (endpoint == Endpoint::Blindbit) {
            requestCapabilities();
        }
    }
    if (!result.is_ok) {
        showError(*serverRow, mapBackendErrorSummary(toQString(result.error)));
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

void Settings::onBackendInfoReady(uint64_t req_id, const BackendInfo &info) {
    if (req_id != m_info_req) {
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
