#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <cstdint>
#include <silent.h>

// A kind of address an account receives on. The silent payment address is static;
// the others are generated one at a time.
enum class AddressKind : std::uint8_t { SilentPayment, Segwit, Taproot };

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

signals:
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);

public slots:
    virtual void updateCoinLabel(const QString &outpoint, const QString &label) = 0;
};
