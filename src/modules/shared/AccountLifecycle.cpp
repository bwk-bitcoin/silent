#include "modules/shared/AccountLifecycle.h"

#include "metatypes.h"

#include <QWidget>
#include <Qontrol>
#include <common.h>

AccountLifecycle::AccountLifecycle(QObject *parent) : QObject(parent) {
    connect(this, &AccountLifecycle::shutdownDrained, this, &AccountLifecycle::onShutdownDrained,
            qontrol::UNIQUE);
    connect(this, &AccountLifecycle::endpointTipFetched, this,
            &AccountLifecycle::onEndpointTipFetched, qontrol::UNIQUE);
    connect(this, &AccountLifecycle::endpointInfoFetched, this,
            &AccountLifecycle::onEndpointInfoFetched, qontrol::UNIQUE);
    connect(this, &AccountLifecycle::electrumProbed, this, &AccountLifecycle::onElectrumProbed,
            qontrol::UNIQUE);
}

void AccountLifecycle::onShutdownDrained(const QString &error) {
    emit shutdownFinished(error);
    emit stopped();
}

auto AccountLifecycle::endpointOperationsBusy() const -> bool {
    return m_deletion_pending || m_account_loading || m_endpoints_stopping || m_settings_busy ||
           m_operation != EndpointOperation::Idle;
}

void AccountLifecycle::setSettingsBusy(bool busy) {
    m_settings_busy = busy;
    publishEndpointState();
    if (!busy) {
        emit settingsIdle();
    }
}

auto AccountLifecycle::settingsBusy() const -> bool {
    return m_settings_busy;
}

auto AccountLifecycle::endpointChangeRefusal() const -> QString {
    return {};
}

void AccountLifecycle::setAccountDeletionPending(bool pending) {
    m_deletion_pending = pending;
    if (auto *widget = qobject_cast<QWidget *>(parent()); widget != nullptr) {
        widget->setEnabled(!pending);
    }
}

auto AccountLifecycle::accountDeletionPending() const -> bool {
    return m_deletion_pending;
}

auto AccountLifecycle::accountLoading() const -> bool {
    return m_account_loading;
}

auto AccountLifecycle::accountLoadError() const -> QString {
    return m_account_load_error;
}

auto AccountLifecycle::hasAccountMetadata() const -> bool {
    return m_config.has_value() && m_config.value()->is_ok();
}

auto AccountLifecycle::accountConfig() const -> const Config & {
    if (!hasAccountMetadata()) {
        qFatal("Account metadata is not loaded");
    }
    return *m_config.value();
}

void AccountLifecycle::setAccountDescriptor(const rust::String &descriptor) {
    m_account_descriptor = descriptor;
}

auto AccountLifecycle::accountDescriptor() const -> rust::String {
    return m_account_descriptor;
}

auto AccountLifecycle::installLoadedConfig() -> QString {
    m_config = std::move(*m_loading_config);
    m_loading_config = nullptr;
    if (hasAccountMetadata()) {
        setAccountDescriptor(accountConfig().get_descriptor());
        setSavedEndpoint(Endpoint::Blindbit, accountConfig().get_blindbit_url(), true);
        setSavedEndpoint(Endpoint::Electrum, accountConfig().get_electrum_url(),
                         accountConfig().get_verify_certificate());
        return {};
    }
    auto error = m_config.value()->get_error();
    return QString::fromUtf8(error.data(), error.size());
}

void AccountLifecycle::finishAccountLoad(const QString &error) {
    m_account_load_error = error;
    m_account_loading = false;
    publishEndpointState();
    emit accountLoadFinished();
}

auto AccountLifecycle::endpointsStopping() const -> bool {
    return m_endpoints_stopping;
}

auto AccountLifecycle::endpointOperation(Endpoint endpoint) const -> EndpointOperation {
    return endpoint == m_operation_endpoint ? m_operation : EndpointOperation::Idle;
}

auto AccountLifecycle::endpointError(Endpoint endpoint) const -> QString {
    return m_endpoint_failures.value(endpoint).error;
}

auto AccountLifecycle::endpointErrorKind(Endpoint endpoint) const -> BackendErrorKind {
    return m_endpoint_failures.value(endpoint).kind;
}

void AccountLifecycle::reportEndpointFailure(Endpoint endpoint, const rust::String &error,
                                             BackendErrorKind kind) {
    if (endpointsStopping()) {
        return;
    }
    m_endpoint_failures.insert(
        endpoint, {.error = QString::fromUtf8(error.data(), error.size()), .kind = kind});
    emit endpointStateChanged(endpoint);
}

void AccountLifecycle::clearEndpointFailure(Endpoint endpoint) {
    if (m_endpoint_failures.remove(endpoint)) {
        emit endpointStateChanged(endpoint);
    }
}

void AccountLifecycle::setSavedEndpoint(Endpoint endpoint, const rust::String &url,
                                        bool verify_certificate) {
    m_saved_endpoint_urls.insert(endpoint, QString::fromUtf8(url.data(), url.size()));
    if (endpoint == Endpoint::Electrum) {
        m_saved_endpoint_verify = verify_certificate;
    }
}

auto AccountLifecycle::savedEndpointUrl(Endpoint endpoint) const -> QString {
    return m_saved_endpoint_urls.value(endpoint);
}

auto AccountLifecycle::savedEndpointVerify() const -> bool {
    return m_saved_endpoint_verify;
}

void AccountLifecycle::publishEndpointState() {
    for (auto endpoint : endpoints()) {
        emit endpointStateChanged(endpoint);
    }
}

void AccountLifecycle::beginEndpointOperation(uint64_t req_id, Endpoint endpoint,
                                              EndpointOperation operation) {
    ++m_endpoint_generation;
    m_operation_req = req_id;
    m_operation_endpoint = endpoint;
    m_operation = operation;
    m_connection_result.reset();
    m_endpoint_failures.remove(endpoint);
    publishEndpointState();
}

void AccountLifecycle::finishEndpointOperation(const EndpointResult &result) {
    if (m_endpoints_stopping) {
        return;
    }
    if (result.is_ok && result.connection_pending) {
        m_connection_result = result;
        publishEndpointState();
        return;
    }
    auto req = m_operation_req;
    auto endpoint = m_operation_endpoint;
    m_operation = EndpointOperation::Idle;
    m_connection_result.reset();
    if (!result.is_ok) {
        reportEndpointFailure(endpoint, result.error, result.kind);
    }
    publishEndpointState();
    emit endpointRequestFinished(req, endpoint, result);
}

void AccountLifecycle::finishEndpointConnection(Endpoint endpoint, bool connected,
                                                const rust::String &error, BackendErrorKind kind) {
    if (!m_connection_result.has_value() || endpoint != m_operation_endpoint) {
        return;
    }
    auto result = *m_connection_result;
    result.connection_pending = false;
    result.is_ok = connected;
    result.error = error;
    result.kind = kind;
    finishEndpointOperation(result);
}

void AccountLifecycle::stopEndpointOperations() {
    if (m_endpoints_stopping) {
        return;
    }
    m_endpoints_stopping = true;
    ++m_endpoint_generation;
    auto operation = m_operation;
    m_operation = EndpointOperation::Idle;
    m_connection_result.reset();
    publishEndpointState();
    if (operation != EndpointOperation::Idle) {
        EndpointResult result{};
        result.kind = BackendErrorKind::Other;
        result.error = rust::String("Account is stopping");
        emit endpointRequestFinished(m_operation_req, m_operation_endpoint, result);
    }
}

void AccountLifecycle::probeElectrum(bool retry) {
    beginEndpointOperation(0, Endpoint::Electrum, EndpointOperation::Connecting);
    startWorker(QThread::create([this, retry, generation = m_endpoint_generation,
                                 url = endpointUrl(Endpoint::Electrum).toStdString(),
                                 verify = verifiesCertificate(), network = network()] {
        emit electrumProbed(
            generation, retry,
            ::test_endpoint(BackendServer::Electrum, rust::String(url), verify, network));
    }));
}

void AccountLifecycle::requestAccountEndpointTip(uint64_t req_id, Endpoint endpoint) {
    auto url = endpointUrl(endpoint).toStdString();
    auto verify = verifiesCertificate();
    startWorker(
        QThread::create([this, req_id, endpoint, url, verify, generation = m_endpoint_generation] {
            ElectrumTipResult result{};
            if (endpoint == Endpoint::Electrum) {
                result = ::electrum_tip_height(rust::String(url), verify);
            } else {
                auto info = ::get_backend_info(rust::String(url));
                result.is_ok = info.is_ok;
                result.error = info.error;
                result.height = info.height;
            }
            emit endpointTipFetched(req_id, generation, endpoint, result);
        }));
}

void AccountLifecycle::requestEndpointInfo(uint64_t req_id, Endpoint endpoint) {
    if (endpoint != Endpoint::Blindbit || endpointOperationsBusy() ||
        endpointUrl(endpoint).isEmpty()) {
        BackendInfo info{};
        info.error = rust::String("Endpoint is unavailable or unsupported");
        emit endpointInfoReady(req_id, endpoint, info);
        return;
    }
    startWorker(QThread::create([this, req_id, endpoint, url = endpointUrl(endpoint).toStdString(),
                                 generation = m_endpoint_generation] {
        emit endpointInfoFetched(req_id, generation, endpoint,
                                 ::get_backend_info(rust::String(url)));
    }));
}

void AccountLifecycle::onEndpointTipFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                                            const ElectrumTipResult &result) {
    if (generation == m_endpoint_generation && !endpointsStopping()) {
        emit endpointTipReady(req_id, endpoint, result);
    }
}

void AccountLifecycle::onEndpointInfoFetched(uint64_t req_id, uint64_t generation,
                                             Endpoint endpoint, const BackendInfo &info) {
    if (generation == m_endpoint_generation && !endpointsStopping()) {
        emit endpointInfoReady(req_id, endpoint, info);
    }
}

void AccountLifecycle::onElectrumProbed(uint64_t generation, bool retry,
                                        const EndpointResult &result) {
    if (generation != m_endpoint_generation || m_endpoints_stopping) {
        return;
    }
    if (!result.is_ok) {
        electrumProbeFailed(retry, result);
        return;
    }
    // The connection runs as its own operation: the state stays Connecting until it starts.
    m_operation = EndpointOperation::Idle;
    connectElectrum(retry);
    // A connection refused meanwhile (a signing round or a deletion started) leaves it idle.
    if (m_operation == EndpointOperation::Idle) {
        publishEndpointState();
    }
}
