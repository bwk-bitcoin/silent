#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <silent.h>

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

signals:
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);

public slots:
    virtual void updateCoinLabel(const QString &outpoint, const QString &label) = 0;
};
