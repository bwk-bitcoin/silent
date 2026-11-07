#pragma once

#include "interfaces/backend/types.h"

#include <QByteArray>
#include <QString>
#include <silent.h>

// Conversions between the Rust bridge types and the account UI service types. Unknown
// values (a zero height, an unknown amount) become empty optionals.
namespace account::services {

auto toQString(const rust::String &value) -> QString;
auto toNetwork(Network network) -> backend::Network;
// `spendable`: a spend path can spend the coin now (its timelock, if any, is met).
auto toCoin(const RustCoin &coin, bool spendable) -> backend::Coin;
auto toBalance(const CoinState &state) -> backend::Balance;
auto toTransaction(const RustTx &tx) -> backend::Transaction;
auto toDetails(const RustTxDetails &details) -> backend::TransactionDetails;
auto toLabel(const LabelEntry &entry) -> backend::Label;
auto toLabelKind(backend::LabelKind kind) -> LabelKind;
auto toAddressKind(const ReceiveKind &kind) -> backend::AddressKind;
auto toAddress(const ReceiveAddress &address) -> backend::Address;
auto toVerification(const ReceiveAddress &address) -> backend::AddressVerification;
auto toTemplate(const backend::TransactionTemplate &transaction) -> TransactionTemplate;
auto toCraftResult(const CraftOutcome &outcome) -> backend::CraftResult;
auto toBroadcastResult(const BroadcastReport &report) -> backend::BroadcastResult;
auto toBytes(const rust::Vec<std::uint8_t> &bytes) -> QByteArray;
auto toSlice(const QByteArray &bytes) -> rust::Slice<const std::uint8_t>;

// Field by field equality, so a publication only signals what changed.
auto same(const backend::Coin &a, const backend::Coin &b) -> bool;
auto same(const backend::Balance &a, const backend::Balance &b) -> bool;
auto same(const backend::Transaction &a, const backend::Transaction &b) -> bool;
auto same(const backend::Label &a, const backend::Label &b) -> bool;
auto same(const backend::Address &a, const backend::Address &b) -> bool;
auto same(const backend::AddressKind &a, const backend::AddressKind &b) -> bool;
auto same(const backend::ScanProgress &a, const backend::ScanProgress &b) -> bool;
auto same(const backend::Connection &a, const backend::Connection &b) -> bool;
auto same(const backend::BlockchainState &a, const backend::BlockchainState &b) -> bool;
auto same(const backend::Endpoint &a, const backend::Endpoint &b) -> bool;
auto same(const backend::Settings &a, const backend::Settings &b) -> bool;

template <typename T> auto same(const QList<T> &a, const QList<T> &b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (!same(a.at(i), b.at(i))) {
            return false;
        }
    }
    return true;
}

} // namespace account::services
