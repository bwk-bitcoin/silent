#pragma once

#include "account/AccountBackend.h"
#include "account/services/ServiceContext.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <silent.h>
#include <utility>

class QThread;

namespace account {

// The service context of a built-in account controller: publishes its funds, labels,
// receive addresses, connection state and endpoint settings as the controller changes
// them, and forwards connection, tip and transaction details requests to it. The
// controller owns it; the module side reads and works on the Rust account.
class ControllerServices : public ServiceContext {
    Q_OBJECT

public:
    ControllerServices(const QString &account, AccountBackend *controller);

    void fetchTransactionDetails(backend::RequestId token, QString txid) override;
    void connectAll(backend::RequestId token, bool start) override;
    void setConnected(backend::RequestId token, QString connection, bool connected) override;
    void refreshTip(backend::RequestId token, QString connection) override;
    void testEndpoint(backend::RequestId token, backend::Endpoint endpoint) override;
    void saveSettings(backend::RequestId token, backend::Settings settings) override;
    void endpointCapabilities(backend::RequestId token, QString endpoint) override;

signals:
    // Worker answers, handled on the GUI thread before the request is answered.
    void endpointTestFinished(backend::RequestId token, EndpointResult result);
    void settingsSaveFinished(backend::RequestId token, EndpointResult result);
    void labelEditFinished(backend::RequestId token, bool ok, bool changed, QString error);
    void addressFinished(backend::RequestId token, bool ok, backend::Address address,
                         QString error);
    void broadcastDone(backend::RequestId token, backend::BroadcastResult result);

public slots:
    void onAccountLoaded();
    void onCoinsUpdated();
    void onEndpointStateChanged();
    void onScanProgress(uint32_t height, uint32_t tip);
    void onTxDetailsReady(uint64_t req_id, const RustTxDetails &details);
    void onEndpointTipReady(uint64_t req_id, Endpoint endpoint, const ElectrumTipResult &result);
    void onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                   const EndpointResult &result);
    void onEndpointInfoReady(uint64_t req_id, Endpoint endpoint, const BackendInfo &info);
    void onEndpointTestFinished(backend::RequestId token, const EndpointResult &result);
    void onSettingsSaveFinished(backend::RequestId token, const EndpointResult &result);
    // The labels are read again first: an edit kept in RAM shows even when its save
    // failed.
    void onLabelEditFinished(backend::RequestId token, bool ok, bool changed, const QString &error);
    void onAddressFinished(backend::RequestId token, bool ok, const backend::Address &address,
                           const QString &error);
    void onBroadcastDone(backend::RequestId token, const backend::BroadcastResult &result);
    void onFundsWorkerFinished();

protected:
    [[nodiscard]] auto controller() const -> AccountBackend *;
    // The account is loaded and neither closing nor being deleted.
    [[nodiscard]] auto available() const -> bool;
    // Starts `worker` on the controller, which waits for it before it drops the account.
    // Answers `token` with failed() instead when the account is not available.
    auto startWork(backend::RequestId token, QThread *worker) -> bool;
    // Runs `read` on a controller worker and answers fundsRead() with its result. False
    // when the account is not available.
    auto startFundsWorker(std::function<std::optional<Funds>()> read) -> bool;
    // available(), and no endpoint operation runs. Answers `token` with failed() otherwise.
    auto endpointsIdle(backend::RequestId token) -> bool;
    [[nodiscard]] auto endpointOf(const QString &connection) const -> std::optional<Endpoint>;
    [[nodiscard]] auto testedUrl(const backend::Endpoint &endpoint) const -> QString;
    void publishEndpoints();
    void connectNext(backend::RequestId token, QList<std::pair<Endpoint, bool>> steps);

private:
    struct Connecting {
        backend::RequestId token = 0;
        // What is left to do once this request answers.
        QList<std::pair<Endpoint, bool>> steps;
    };

    struct Tip {
        backend::RequestId token = 0;
        Endpoint endpoint = Endpoint::Electrum;
    };

    QPointer<AccountBackend> m_controller;
    QHash<uint64_t, backend::RequestId> m_details_reqs;
    QHash<uint64_t, Connecting> m_connect_reqs;
    QHash<uint64_t, Tip> m_tip_reqs;
    QHash<uint64_t, backend::RequestId> m_info_reqs;
    QHash<backend::RequestId, backend::Settings> m_saving;
    QHash<backend::RequestId, backend::Endpoint> m_testing;
    // The url each endpoint last passed a test with, and the one the test found: the same
    // server on its working transport.
    QHash<QString, std::pair<QString, QString>> m_tested;
    QHash<Endpoint, quint64> m_tips;
    std::shared_ptr<std::optional<Funds>> m_funds_result;
    std::optional<quint64> m_scan_height;
    std::optional<quint64> m_scan_target;
};

} // namespace account
