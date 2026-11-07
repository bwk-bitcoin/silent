#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>
#include <optional>

// The amount and fee rules of the account views, in Qt types: what a payment, a fee or a
// set of coins come to. Pure, the same for every account.
namespace account::money {

struct Amounts {
    quint64 inputTotal{};
    quint64 sendingTotal{};
    // What the coins lack to pay the recipients.
    quint64 remaining{};
    quint64 change{};
    // The recipients and the fee.
    quint64 transactionTotal{};
};

// What coins worth `inputs` pay to recipients of `recipients`, with `fee`.
auto amounts(const QList<quint64> &inputs, const QList<quint64> &recipients, quint64 fee)
    -> Amounts;

// The sats `btc` states, a decimal with a '.' point; nullopt when it is no amount.
auto parseBtc(const QString &btc) -> std::optional<quint64>;

struct Fee {
    double rate{};
    // Nonzero for an absolute fee in sats, which overrides the rate.
    quint64 absolute{};
};

// The fee `text` states, a rate in sat/vB when `rate`, else an absolute fee in sats.
auto parseFee(const QString &text, bool rate) -> std::optional<Fee>;

// `rate` as a fee field takes it, nullopt when it is no fee rate.
auto normalizeFeeRate(double rate) -> std::optional<double>;

// Whether `address` is an address a payment can go to.
auto isAddress(const QString &address) -> bool;

} // namespace account::money
