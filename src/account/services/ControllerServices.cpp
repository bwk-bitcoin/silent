#include "account/services/ControllerServices.h"

#include "account/RequestId.h"
#include "account/services/Conversions.h"
#include "metatypes.h"

#include <QThread>
#include <Qontrol>
#include <common.h>
#include <utility>

namespace account {
namespace {

auto connectionId(Endpoint endpoint) -> QString {
    return endpoint == Endpoint::Blindbit ? QStringLiteral("blindbit") : QStringLiteral("electrum");
}

auto connectionName(Endpoint endpoint) -> QString {
    return endpoint == Endpoint::Blindbit ? QStringLiteral("BlindBit") : QStringLiteral("Electrum");
}

auto server(Endpoint endpoint) -> BackendServer {
    return endpoint == Endpoint::Blindbit ? BackendServer::Blindbit : BackendServer::Electrum;
}

auto unavailable() -> QString {
    return QStringLiteral("Account is not available");
}

} // namespace

ControllerServices::ControllerServices(const QString &account, AccountBackend *controller)
    : ServiceContext(account, controller),
      m_controller(controller) {
    connect(controller, &AccountBackend::accountLoadFinished, this,
            &ControllerServices::onAccountLoaded, qontrol::UNIQUE);
    connect(controller, &AccountBackend::updateCoins, this, &ControllerServices::onCoinsUpdated,
            qontrol::UNIQUE);
    connect(controller, &AccountBackend::labelsChanged, this, &ServiceContext::onLabelsChanged,
            qontrol::UNIQUE);
    connect(controller, &AccountBackend::endpointStateChanged, this,
            &ControllerServices::onEndpointStateChanged, qontrol::UNIQUE);
    connect(controller, &AccountBackend::scanProgress, this, &ControllerServices::onScanProgress,
            qontrol::UNIQUE);
    connect(controller, &AccountBackend::txDetailsReady, this,
            &ControllerServices::onTxDetailsReady, qontrol::UNIQUE);
    connect(controller, &AccountBackend::endpointTipReady, this,
            &ControllerServices::onEndpointTipReady, qontrol::UNIQUE);
    connect(controller, &AccountBackend::endpointRequestFinished, this,
            &ControllerServices::onEndpointRequestFinished, qontrol::UNIQUE);
    connect(this, &ControllerServices::endpointTestFinished, this,
            &ControllerServices::onEndpointTestFinished, qontrol::UNIQUE);
    connect(this, &ControllerServices::settingsSaveFinished, this,
            &ControllerServices::onSettingsSaveFinished, qontrol::UNIQUE);
    connect(this, &ControllerServices::labelEditFinished, this,
            &ControllerServices::onLabelEditFinished, qontrol::UNIQUE);
    connect(this, &ControllerServices::addressFinished, this,
            &ControllerServices::onAddressFinished, qontrol::UNIQUE);
    connect(this, &ControllerServices::broadcastDone, this, &ControllerServices::onBroadcastDone,
            qontrol::UNIQUE);
}

auto ControllerServices::controller() const -> AccountBackend * {
    return m_controller.data();
}

auto ControllerServices::available() const -> bool {
    return m_controller != nullptr && m_controller->hasAccountMetadata() &&
           !m_controller->accountLoading() && !m_controller->endpointsStopping() &&
           !m_controller->accountDeletionPending();
}

auto ControllerServices::endpointsIdle(backend::RequestId token) -> bool {
    if (!available()) {
        emit failed(token, unavailable());
        return false;
    }
    // An endpoint change reads and writes the same config, then records what it saved.
    if (m_controller->endpointOperationsBusy()) {
        emit failed(token, QStringLiteral("An endpoint operation is in progress"));
        return false;
    }
    return true;
}

auto ControllerServices::startWork(backend::RequestId token, QThread *worker) -> bool {
    if (!available()) {
        worker->deleteLater();
        emit failed(token, unavailable());
        return false;
    }
    m_controller->startWorker(worker);
    return true;
}

auto ControllerServices::endpointOf(const QString &connection) const -> std::optional<Endpoint> {
    if (m_controller == nullptr) {
        return std::nullopt;
    }
    for (auto endpoint : m_controller->endpoints()) {
        if (connectionId(endpoint) == connection) {
            return endpoint;
        }
    }
    return std::nullopt;
}

void ControllerServices::publishEndpoints() {
    if (m_controller == nullptr || !m_controller->hasAccountMetadata()) {
        return;
    }
    backend::BlockchainState state{.error = m_controller->accountLoadError()};
    backend::Settings settings{.network = services::toNetwork(m_controller->network())};
    for (auto endpoint : m_controller->endpoints()) {
        auto tip = m_tips.constFind(endpoint);
        state.connections.append(
            {.id = connectionId(endpoint),
             .name = connectionName(endpoint),
             .connected = m_controller->isEndpointRunning(endpoint),
             .tip = tip == m_tips.cend() ? std::nullopt : std::optional<quint64>(tip.value()),
             .error = m_controller->endpointError(endpoint)});
        settings.endpoints.append({.id = connectionId(endpoint),
                                   .name = connectionName(endpoint),
                                   .url = m_controller->endpointUrl(endpoint),
                                   .verifyCertificate = endpoint != Endpoint::Electrum ||
                                                        m_controller->verifiesCertificate()});
    }
    if (m_scan_target.has_value()) {
        state.progress.append({.id = connectionId(Endpoint::Blindbit),
                               .name = connectionName(Endpoint::Blindbit),
                               .height = m_scan_height,
                               .target = m_scan_target});
        state.scanning =
            m_controller->isEndpointRunning(Endpoint::Blindbit) && m_scan_height < m_scan_target;
        state.tip = m_scan_target;
    } else if (auto tip = m_tips.constFind(Endpoint::Electrum); tip != m_tips.cend()) {
        state.tip = tip.value();
    }
    publishNetwork(settings.network);
    publishState(state);
    publishSettings(settings);
}

void ControllerServices::fetchTransactionDetails(backend::RequestId token, QString txid) {
    if (!available()) {
        emit failed(token, unavailable());
        return;
    }
    auto req = nextRequestId();
    // The answer may come during the call.
    m_details_reqs.insert(req, token);
    m_controller->requestTxDetails(req, txid);
}

void ControllerServices::connectAll(backend::RequestId token, bool start) {
    if (!available()) {
        emit failed(token, unavailable());
        return;
    }
    QList<std::pair<Endpoint, bool>> steps;
    for (auto endpoint : m_controller->endpoints()) {
        steps.append({endpoint, start});
    }
    connectNext(token, steps);
}

void ControllerServices::connectNext(backend::RequestId token,
                                     QList<std::pair<Endpoint, bool>> steps) {
    if (steps.isEmpty()) {
        emit completed(token);
        return;
    }
    if (!available()) {
        emit failed(token, unavailable());
        return;
    }
    auto [endpoint, connected] = steps.takeFirst();
    auto req = nextRequestId();
    m_connect_reqs.insert(req, {.token = token, .steps = steps});
    m_controller->setEndpointConnected(req, endpoint, connected);
}

void ControllerServices::setConnected(backend::RequestId token, QString connection,
                                      bool connected) {
    auto endpoint = endpointOf(connection);
    if (!endpoint.has_value()) {
        emit failed(token, QStringLiteral("Unknown connection '%1'").arg(connection));
        return;
    }
    connectNext(token, {{*endpoint, connected}});
}

void ControllerServices::refreshTip(backend::RequestId token, QString connection) {
    auto endpoint = endpointOf(connection);
    if (!endpoint.has_value()) {
        emit failed(token, QStringLiteral("Unknown connection '%1'").arg(connection));
        return;
    }
    if (!available()) {
        emit failed(token, unavailable());
        return;
    }
    auto req = nextRequestId();
    m_tip_reqs.insert(req, {.token = token, .endpoint = *endpoint});
    m_controller->requestEndpointTip(req, *endpoint);
}

void ControllerServices::testEndpoint(backend::RequestId token, backend::Endpoint endpoint) {
    auto known = endpointOf(endpoint.id);
    if (!known.has_value()) {
        emit failed(token, QStringLiteral("Unknown endpoint '%1'").arg(endpoint.id));
        return;
    }
    if (!endpointsIdle(token)) {
        return;
    }
    auto network = m_controller->network();
    startWork(token,
              QThread::create([this, token, kind = server(*known), url = endpoint.url.toStdString(),
                               verify = endpoint.verifyCertificate, network] {
                  auto result = ::test_endpoint(kind, rust::String(url), verify, network);
                  emit endpointTestFinished(token, result.is_ok, services::toQString(result.error));
              }));
}

void ControllerServices::saveSettings(backend::RequestId token, backend::Settings settings) {
    if (!endpointsIdle(token)) {
        return;
    }
    if (settings.network != services::toNetwork(m_controller->network())) {
        emit failed(token, QStringLiteral("The network of this account cannot change"));
        return;
    }
    rust::Vec<EndpointSetting> endpoints;
    for (const auto &endpoint : settings.endpoints) {
        auto known = endpointOf(endpoint.id);
        if (!known.has_value()) {
            emit failed(token, QStringLiteral("Unknown endpoint '%1'").arg(endpoint.id));
            return;
        }
        endpoints.push_back({.server = server(*known),
                             .url = rust::String(endpoint.url.toStdString()),
                             .verify_certificate = endpoint.verifyCertificate});
    }
    m_saving.insert(token, settings);
    m_controller->setSettingsSaving(true);
    if (!startWork(token, QThread::create([this, token, account = this->account().toStdString(),
                                           endpoints = std::move(endpoints)]() mutable {
                       auto result = ::save_endpoints(rust::String(account), std::move(endpoints));
                       emit settingsSaveFinished(token, result.is_ok,
                                                 services::toQString(result.error));
                   }))) {
        m_saving.remove(token);
        m_controller->setSettingsSaving(false);
    }
}

// ===== Slots =====

void ControllerServices::onAccountLoaded() {
    publishEndpoints();
    onFundsChanged();
    onLabelsChanged();
    onReceivingChanged();
}

void ControllerServices::onCoinsUpdated() {
    onFundsChanged();
    // A new address tip reaches the controller as a coin poll.
    onReceivingChanged();
}

void ControllerServices::onEndpointStateChanged() {
    publishEndpoints();
}

void ControllerServices::onScanProgress(uint32_t height, uint32_t tip) {
    m_scan_height = height;
    m_scan_target = tip;
    publishEndpoints();
}

void ControllerServices::onTxDetailsReady(uint64_t req_id, const RustTxDetails &details) {
    auto found = m_details_reqs.find(req_id);
    if (found == m_details_reqs.end()) {
        return;
    }
    auto token = found.value();
    m_details_reqs.erase(found);
    if (!details.is_ok) {
        emit failed(token, services::toQString(details.error));
        return;
    }
    auto converted = services::toDetails(details);
    publishDetails(converted);
    emit transactionDetailsFetched(token, converted);
}

void ControllerServices::onEndpointTipReady(uint64_t req_id, [[maybe_unused]] Endpoint endpoint,
                                            const ElectrumTipResult &result) {
    auto found = m_tip_reqs.find(req_id);
    if (found == m_tip_reqs.end()) {
        return;
    }
    auto tip = found.value();
    m_tip_reqs.erase(found);
    if (!result.is_ok) {
        emit failed(tip.token, services::toQString(result.error));
        return;
    }
    m_tips.insert(tip.endpoint, result.height);
    publishEndpoints();
    emit completed(tip.token);
}

void ControllerServices::onEndpointRequestFinished(uint64_t req_id,
                                                   [[maybe_unused]] Endpoint endpoint,
                                                   const EndpointResult &result) {
    publishEndpoints();
    auto found = m_connect_reqs.find(req_id);
    if (found == m_connect_reqs.end()) {
        return;
    }
    auto connecting = found.value();
    m_connect_reqs.erase(found);
    if (!result.is_ok) {
        emit failed(connecting.token, services::toQString(result.error));
        return;
    }
    connectNext(connecting.token, connecting.steps);
}

void ControllerServices::onEndpointTestFinished(backend::RequestId token, bool ok,
                                                const QString &error) {
    if (ok) {
        emit endpointTested(token);
    } else {
        emit failed(token, error);
    }
}

void ControllerServices::onSettingsSaveFinished(backend::RequestId token, bool ok,
                                                const QString &error) {
    auto settings = m_saving.take(token);
    if (m_controller != nullptr) {
        m_controller->setSettingsSaving(false);
    }
    if (!ok) {
        emit failed(token, error);
        return;
    }
    if (m_controller != nullptr) {
        for (const auto &endpoint : settings.endpoints) {
            if (auto known = endpointOf(endpoint.id); known.has_value()) {
                m_controller->setSavedEndpoint(*known,
                                               rust::String(endpoint.url.trimmed().toStdString()),
                                               endpoint.verifyCertificate);
            }
        }
    }
    publishEndpoints();
    emit completed(token);
}

void ControllerServices::onLabelEditFinished(backend::RequestId token, bool ok, bool changed,
                                             const QString &error) {
    if (changed) {
        onLabelsChanged();
        // Coins and history carry their labels.
        onFundsChanged();
    }
    if (ok) {
        emit labelSaved(token);
    } else {
        emit failed(token, error);
    }
}

void ControllerServices::onAddressFinished(backend::RequestId token, bool ok,
                                           const backend::Address &address, const QString &error) {
    if (!ok) {
        emit failed(token, error);
        return;
    }
    onReceivingChanged();
    emit addressCreated(token, address);
}

void ControllerServices::onBroadcastDone(backend::RequestId token,
                                         const backend::BroadcastResult &result) {
    if (result.outcome == backend::BroadcastOutcome::Accepted) {
        onFundsChanged();
    }
    emit broadcastFinished(token, result);
}

} // namespace account
