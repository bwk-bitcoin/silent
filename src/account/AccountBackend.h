#pragma once

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThread>
#include <cstdint>
#include <silent.h>

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

// What the account panels ask of their account, whatever its kind: the sp account
// and the musig account controllers both implement it, so one set of panels serves
// both.
class AccountBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
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
    // false when it could not start.
    virtual auto startEndpoint(Endpoint endpoint) -> bool = 0;
    virtual void stopEndpoint(Endpoint endpoint) = 0;
    // Whether Settings offers to move the account to another network.
    [[nodiscard]] virtual auto networkEditable() const -> bool = 0;
    // Saves the endpoint urls and the network; why it failed, empty once saved.
    virtual auto saveSettings(const QHash<Endpoint, QString> &urls, Network network) -> QString = 0;
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
    // A passing test persists the endpoint, then leaves that endpoint disconnected.
    virtual void testAndSaveEndpoint(uint64_t req_id, Endpoint endpoint, const QString &url,
                                     bool verify_certificate) = 0;
    virtual void setEndpointConnected(uint64_t req_id, Endpoint endpoint, bool connected) = 0;

signals:
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
    void endpointRequestFinished(uint64_t req_id, Endpoint endpoint, EndpointResult result);

public slots:
    virtual void updateCoinLabel(const QString &outpoint, const QString &label) = 0;

protected:
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

    void requestAccountEndpointTip(uint64_t req_id, Endpoint endpoint, const QString &name) {
        startWorker(QThread::create([this, req_id, endpoint, name = name.toStdString()] {
            ElectrumTipResult result;
            result.is_ok = false;
            auto config = config_from_file(rust::String(name));
            if (!config->is_ok()) {
                result.error = config->get_error();
            } else if (endpoint == Endpoint::Electrum) {
                result = electrum_tip_height(config->get_electrum_url(),
                                             config->get_verify_certificate());
            } else {
                auto info = get_backend_info(config->get_blindbit_url());
                result.is_ok = info.is_ok;
                result.error = info.error;
                result.height = info.height;
            }
            emit endpointTipReady(req_id, endpoint, result);
        }));
    }
};
