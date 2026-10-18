#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <cstdint>
#include <silent.h>

class QThread;
class QWidget;

namespace account {
class SendSigning;
} // namespace account

// A kind of address an account receives on. The silent payment address is static;
// the others are generated one at a time.
enum class AddressKind : std::uint8_t { SilentPayment, Segwit, Taproot };

// A server an account syncs from.
enum class Endpoint : std::uint8_t { Blindbit, Electrum };

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

signals:
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);
    void scanProgress(uint32_t height, uint32_t tip);
    // `endpoint` started or stopped.
    void endpointStateChanged(Endpoint endpoint);

public slots:
    virtual void updateCoinLabel(const QString &outpoint, const QString &label) = 0;
};
