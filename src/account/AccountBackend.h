#pragma once

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThread>
#include <cstdint>
#include <memory>
#include <optional>
#include <silent.h>
#include <utility>

class IModule;

namespace account {
class ServiceContext;
} // namespace account

// A kind of address a built-in account receives on. The silent payment address is
// static; the others are generated one at a time.
enum class AddressKind : std::uint8_t { SilentPayment, Segwit, Taproot };

// A server an account syncs from.
enum class Endpoint : std::uint8_t { Blindbit, Electrum };
Q_DECLARE_METATYPE(Endpoint)

enum class EndpointOperation : std::uint8_t { Idle, Connecting, Disconnecting };

// What the built-in sp and musig account controllers share: the account's lifecycle,
// its endpoint connections and the requests behind its UI services
// (account::ControllerServices), and what the status bars read. The account panels use
// the services, not this class.
class AccountBackend : public QObject {
    Q_OBJECT

public:
    explicit AccountBackend(QObject *parent = nullptr);
    ~AccountBackend() override = default;

    // The account's network, the one the keys it signs with are registered on.
    virtual auto network() -> Network = 0;
    // The module the account belongs to: the caller its panels name to the host.
    virtual auto module() -> IModule * = 0;

    // The servers the account syncs from, in the order Settings lists them.
    virtual auto endpoints() -> QList<Endpoint> = 0;
    // The url saved for `endpoint`, empty when none is.
    virtual auto endpointUrl(Endpoint endpoint) -> QString = 0;
    // Whether the Electrum server's TLS certificate is checked: off only for a
    // self-signed server the account was created against.
    [[nodiscard]] virtual auto verifiesCertificate() const -> bool = 0;
    [[nodiscard]] virtual auto isEndpointRunning(Endpoint endpoint) const -> bool = 0;
    [[nodiscard]] auto endpointOperationsBusy() const -> bool;
    // A settings service test or save runs: endpoint operations wait for it. Publishes the
    // endpoint state; settingsIdle() follows its end.
    void setSettingsBusy(bool busy);
    [[nodiscard]] auto settingsBusy() const -> bool;
    // Why the account would refuse an endpoint change now, besides a running endpoint
    // operation, empty when it would not. A settings test or save is refused for it too,
    // so the saved endpoint never differs from the live account's.
    [[nodiscard]] virtual auto endpointChangeRefusal() const -> QString;
    [[nodiscard]] auto endpointsStopping() const -> bool;
    [[nodiscard]] auto endpointOperation(Endpoint endpoint) const -> EndpointOperation;
    [[nodiscard]] auto endpointError(Endpoint endpoint) const -> QString;
    [[nodiscard]] auto endpointErrorKind(Endpoint endpoint) const -> BackendErrorKind;
    [[nodiscard]] auto accountLoading() const -> bool;
    [[nodiscard]] auto accountLoadError() const -> QString;
    void setAccountDeletionPending(bool pending);
    [[nodiscard]] auto accountDeletionPending() const -> bool;
    [[nodiscard]] auto hasAccountMetadata() const -> bool;
    // The state and requests behind the account's UI services, a child of this backend.
    // Null for a backend serving none, such as a mock account.
    [[nodiscard]] virtual auto services() const -> account::ServiceContext *;
    // Records `url` as the saved endpoint, as the account's config now holds it.
    void setSavedEndpoint(Endpoint endpoint, const rust::String &url, bool verify_certificate);

    // Starts a worker thread that uses the account or a view: the account outlives it.
    virtual void startWorker(QThread *worker) = 0;
    virtual void requestTxDetails(uint64_t req_id, const QString &txid) = 0;
    virtual void requestEndpointTip(uint64_t req_id, Endpoint endpoint) = 0;
    void requestEndpointInfo(uint64_t req_id, Endpoint endpoint);
    virtual void setEndpointConnected(uint64_t req_id, Endpoint endpoint, bool connected) = 0;

signals:
    void stopped();
    // A settings test or save ended: a change it held back can run.
    void settingsIdle();
    void shutdownFinished(QString error);
    void shutdownDrained(QString error);
    void accountLoadFinished();
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);
    void scanProgress(uint32_t height, uint32_t tip);
    // `endpoint` started or stopped.
    void endpointStateChanged(Endpoint endpoint);
    void txDetailsReady(uint64_t req_id, RustTxDetails details);
    void endpointTipReady(uint64_t req_id, Endpoint endpoint, ElectrumTipResult result);
    // Connecting completes on backend confirmation. Later connection losses update endpoint state.
    void endpointRequestFinished(uint64_t req_id, Endpoint endpoint, EndpointResult result);
    void endpointInfoReady(uint64_t req_id, Endpoint endpoint, BackendInfo info);
    void endpointTipFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                            ElectrumTipResult result);
    void endpointInfoFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                             BackendInfo info);

public slots:
    void onShutdownDrained(const QString &error);
    void onEndpointTipFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                              const ElectrumTipResult &result);
    void onEndpointInfoFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                               const BackendInfo &info);

protected:
    template <typename T>
    void drainAccount(std::optional<rust::Box<T>> account, QThread *notif,
                      QList<QThread *> workers) {
        auto *teardown = QThread::create(
            [this, acc = std::move(account), notif, workers = std::move(workers)]() mutable {
                for (auto *worker : workers) {
                    worker->wait();
                    worker->deleteLater();
                }
                QString error;
                if (acc.has_value()) {
                    auto failure = acc.value()->shutdown_backend();
                    error = QString::fromUtf8(failure.data(), failure.size());
                    acc.reset();
                }
                if (notif != nullptr) {
                    notif->wait();
                    notif->deleteLater();
                }
                emit shutdownDrained(error);
            });
        connect(teardown, &QThread::finished, teardown, &QThread::deleteLater,
                Qt::UniqueConnection);
        teardown->start();
    }

    void beginEndpointOperation(uint64_t req_id, Endpoint endpoint, EndpointOperation operation);
    void finishEndpointOperation(const EndpointResult &result);
    void finishEndpointConnection(Endpoint endpoint, bool connected, const rust::String &error,
                                  BackendErrorKind kind);
    void stopEndpointOperations();
    void publishEndpointState();
    void requestAccountEndpointTip(uint64_t req_id, Endpoint endpoint);
    void reportEndpointFailure(Endpoint endpoint, const rust::String &error, BackendErrorKind kind);
    void clearEndpointFailure(Endpoint endpoint);
    [[nodiscard]] auto savedEndpointUrl(Endpoint endpoint) const -> QString;
    [[nodiscard]] auto savedEndpointVerify() const -> bool;
    auto installLoadedConfig() -> QString;
    void finishAccountLoad(const QString &error);
    [[nodiscard]] auto accountConfig() const -> const Config &;
    void setAccountDescriptor(const rust::String &descriptor);
    [[nodiscard]] auto accountDescriptor() const -> rust::String;

    template <typename T, typename Open>
    auto accountLoadWorker(const QString &name,
                           std::shared_ptr<std::optional<rust::Box<T>>> account, Open open)
        -> QThread * {
        m_account_loading = true;
        m_account_load_error.clear();
        m_loading_config = std::make_shared<std::optional<rust::Box<Config>>>();
        return QThread::create([config = m_loading_config, account = std::move(account), open,
                                name = name.toStdString()] {
            config->emplace(::config_from_file(rust::String(name)));
            if (config->value()->is_ok()) {
                account->emplace(open(*config->value()));
            }
        });
    }

    template <typename T, typename Create, typename Take>
    auto endpointChangeWorker(std::shared_ptr<std::optional<rust::Box<T>>> account,
                              std::shared_ptr<EndpointResult> result,
                              std::shared_ptr<bool> replaced, QList<QThread *> workers,
                              QThread *notification_thread, Create create, Take take,
                              std::shared_ptr<BackendServer> server = nullptr) -> QThread * {
        return QThread::create([account = std::move(account), result = std::move(result),
                                replaced = std::move(replaced), workers = std::move(workers),
                                notification_thread, create = std::move(create),
                                take = std::move(take), server = std::move(server)] {
            for (auto *worker : workers) {
                worker->wait();
                worker->deleteLater();
            }
            auto change = create(std::move(account->value()));
            change->prepare_endpoint();
            if (change->endpoint_phase() == EndpointPhase::Draining) {
                *replaced = true;
                if (notification_thread != nullptr) {
                    notification_thread->wait();
                    notification_thread->deleteLater();
                }
                change->finish_endpoint_after_drain();
            }
            *result = change->endpoint_result();
            if (server != nullptr) {
                *server = change->endpoint_server();
            }
            account->value() = take(std::move(change));
        });
    }

    template <typename T>
    void requestAccountTxDetails(uint64_t req_id, const QString &txid, T *account, bool online) {
        if (account == nullptr || endpointsStopping() || accountDeletionPending()) {
            RustTxDetails result;
            result.is_ok = false;
            result.error = rust::String("Account is not available");
            emit txDetailsReady(req_id, result);
            return;
        }
        startWorker(QThread::create([this, account, req_id, id = txid.toStdString(), online] {
            emit txDetailsReady(req_id, account->tx_details(rust::Str(id), online));
        }));
    }

private:
    struct EndpointFailure {
        QString error;
        BackendErrorKind kind = BackendErrorKind::None;
    };

    QHash<Endpoint, EndpointFailure> m_endpoint_failures;
    QHash<Endpoint, QString> m_saved_endpoint_urls;
    bool m_saved_endpoint_verify = true;
    bool m_endpoints_stopping = false;
    uint64_t m_endpoint_generation = 0;
    uint64_t m_operation_req = 0;
    Endpoint m_operation_endpoint = Endpoint::Electrum;
    EndpointOperation m_operation = EndpointOperation::Idle;
    std::optional<EndpointResult> m_connection_result;
    std::optional<rust::Box<Config>> m_config;
    rust::String m_account_descriptor;
    std::shared_ptr<std::optional<rust::Box<Config>>> m_loading_config;
    bool m_account_loading = false;
    bool m_deletion_pending = false;
    bool m_settings_busy = false;
    QString m_account_load_error;
};
