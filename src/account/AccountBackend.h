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
class QWidget;

namespace account {
class SendSigning;
} // namespace account

// A kind of address an account receives on. The silent payment address is static;
// the others are generated one at a time.
enum class AddressKind : std::uint8_t { SilentPayment, Segwit, Taproot };

// A server an account syncs from.
enum class Endpoint : std::uint8_t { Blindbit, Electrum };
Q_DECLARE_METATYPE(Endpoint)

enum class EndpointOperation : std::uint8_t { Idle, Testing, Connecting, Disconnecting };

// What the account panels ask of their account, whatever its kind: the sp account
// and the musig account controllers both implement it, so one set of panels serves
// both.
class AccountBackend : public QObject {
    Q_OBJECT

public:
    explicit AccountBackend(QObject *parent = nullptr);
    ~AccountBackend() override = default;

    // Unspent coins only.
    virtual auto getCoins() -> rust::Vec<RustCoin> = 0;
    virtual auto getPaymentHistory() -> rust::Vec<RustTx> = 0;
    // The account's network, the one the keys it signs with are registered on.
    virtual auto network() -> Network = 0;
    // The module the account belongs to: the caller its panels name to the host.
    virtual auto module() -> IModule * = 0;
    // The kinds of address the account receives on, in the order Receive lists them.
    virtual auto addressKinds() -> QList<AddressKind> = 0;
    // A new address of `kind`, the static one for SilentPayment. Empty on failure.
    virtual auto newAddress(AddressKind kind) -> QString = 0;
    // The descriptor and derivation path a device needs to show `address`.
    virtual auto addressVerifyInfo(const QString &address) -> AddressVerifyInfo = 0;

    // The servers the account syncs from, in the order Settings lists them.
    virtual auto endpoints() -> QList<Endpoint> = 0;
    // The url saved for `endpoint`, empty when none is.
    virtual auto endpointUrl(Endpoint endpoint) -> QString = 0;
    // Whether the Electrum server's TLS certificate is checked: off only for a
    // self-signed server the account was created against.
    [[nodiscard]] virtual auto verifiesCertificate() const -> bool = 0;
    [[nodiscard]] virtual auto isEndpointRunning(Endpoint endpoint) const -> bool = 0;
    [[nodiscard]] auto endpointOperationsBusy() const -> bool;
    [[nodiscard]] auto endpointsStopping() const -> bool;
    [[nodiscard]] auto endpointOperation(Endpoint endpoint) const -> EndpointOperation;
    [[nodiscard]] auto endpointError(Endpoint endpoint) const -> QString;
    [[nodiscard]] auto endpointErrorKind(Endpoint endpoint) const -> BackendErrorKind;
    [[nodiscard]] auto accountLoading() const -> bool;
    [[nodiscard]] auto accountLoadError() const -> QString;
    [[nodiscard]] auto hasAccountMetadata() const -> bool;
    // What Settings shows below the endpoints, nullptr when nothing. Owned by the caller.
    virtual auto settingsExtras() -> QWidget * = 0;

    // Fee, inputs and validity of `tx`, without building it.
    virtual auto simulateTx(TransactionTemplate tx) -> TransactionSimulation = 0;
    // Send's signing step for this account. Owned by the caller.
    virtual auto sendSigning() -> account::SendSigning * = 0;

    // Starts a worker thread that uses the account or a view: the account outlives it.
    virtual void startWorker(QThread *worker) = 0;
    virtual void requestTxDetails(uint64_t req_id, const QString &txid) = 0;
    virtual void editTxLabel(uint64_t req_id, const QString &txid, const QString &label) = 0;
    virtual void editOutpointLabel(uint64_t req_id, const QString &outpoint,
                                   const QString &label) = 0;
    virtual void requestEndpointTip(uint64_t req_id, Endpoint endpoint) = 0;
    void requestEndpointInfo(uint64_t req_id, Endpoint endpoint);
    // A passing test persists the endpoint, then leaves that endpoint disconnected.
    virtual void testAndSaveEndpoint(uint64_t req_id, Endpoint endpoint, const QString &url,
                                     bool verify_certificate) = 0;
    virtual void setEndpointConnected(uint64_t req_id, Endpoint endpoint, bool connected) = 0;

signals:
    void accountLoadFinished();
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);
    void scanProgress(uint32_t height, uint32_t tip);
    // `endpoint` started or stopped.
    void endpointStateChanged(Endpoint endpoint);
    void txDetailsReady(uint64_t req_id, RustTxDetails details);
    // True only once the label is saved; a failed flush retains the RAM edit.
    void labelEdited(uint64_t req_id, bool accepted, QString error);
    void labelsChanged();
    void endpointTipReady(uint64_t req_id, Endpoint endpoint, ElectrumTipResult result);
    // Connecting completes on backend confirmation. Later connection losses update endpoint state.
    void endpointRequestFinished(uint64_t req_id, Endpoint endpoint, EndpointResult result);
    void endpointInfoReady(uint64_t req_id, Endpoint endpoint, BackendInfo info);
    void endpointTipFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                            ElectrumTipResult result);
    void endpointInfoFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                             BackendInfo info);

public slots:
    virtual void updateCoinLabel(const QString &outpoint, const QString &label) = 0;
    void onEndpointTipFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                              const ElectrumTipResult &result);
    void onEndpointInfoFetched(uint64_t req_id, uint64_t generation, Endpoint endpoint,
                               const BackendInfo &info);

protected:
    void beginEndpointOperation(uint64_t req_id, Endpoint endpoint, EndpointOperation operation);
    void finishEndpointOperation(const EndpointResult &result);
    void finishEndpointConnection(Endpoint endpoint, bool connected, const rust::String &error,
                                  BackendErrorKind kind);
    void stopEndpointOperations();
    void publishEndpointState();
    void requestAccountEndpointTip(uint64_t req_id, Endpoint endpoint);
    void setSavedEndpoint(Endpoint endpoint, const rust::String &url, bool verify_certificate);
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
        return QThread::create(
            [config = m_loading_config, account, open, name = name.toStdString()] {
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
        return QThread::create(
            [account, result, replaced, workers, notification_thread, create, take, server] {
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
        if (account == nullptr) {
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

    template <typename T>
    void editAccountLabel(uint64_t req_id, const QString &id, const QString &label, T *account,
                          bool transaction) {
        if (account == nullptr) {
            emit labelEdited(req_id, false, QStringLiteral("Account is not available"));
            return;
        }
        startWorker(QThread::create([this, account, req_id, key = id.toStdString(),
                                     value = label.toStdString(), transaction] {
            auto result = transaction
                              ? account->update_tx_label(rust::String(key), rust::String(value))
                              : account->update_coin_label(rust::String(key), rust::String(value));
            emit labelEdited(req_id, result.is_ok,
                             QString::fromUtf8(result.error.data(), result.error.size()));
            // A failed flush keeps the label in RAM; refresh without reporting it saved.
            emit labelsChanged();
            emit updateCoins(account->spendable_coins());
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
    QString m_account_load_error;
};
