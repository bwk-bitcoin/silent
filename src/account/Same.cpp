#include "account/Same.h"

namespace account {

auto same(const backend::Coin &a, const backend::Coin &b) -> bool {
    return a.outpoint == b.outpoint && a.amount == b.amount && a.height == b.height &&
           a.label == b.label && a.origin == b.origin && a.spent == b.spent &&
           a.spendable == b.spendable;
}

auto same(const backend::Balance &a, const backend::Balance &b) -> bool {
    return a.confirmed == b.confirmed && a.unconfirmed == b.unconfirmed &&
           a.confirmedCount == b.confirmedCount && a.unconfirmedCount == b.unconfirmedCount;
}

auto same(const backend::Transaction &a, const backend::Transaction &b) -> bool {
    return a.txid == b.txid && a.direction == b.direction && a.status == b.status &&
           a.label == b.label && a.amount == b.amount && a.fee == b.fee && a.height == b.height &&
           a.timestamp == b.timestamp;
}

auto same(const backend::Label &a, const backend::Label &b) -> bool {
    return a.key.kind == b.key.kind && a.key.id == b.key.id && a.text == b.text;
}

auto same(const backend::Address &a, const backend::Address &b) -> bool {
    return a.kind == b.kind && a.address == b.address;
}

auto same(const backend::AddressKind &a, const backend::AddressKind &b) -> bool {
    return a.id == b.id && a.name == b.name && a.reusable == b.reusable;
}

auto same(const backend::Connection &a, const backend::Connection &b) -> bool {
    return a.id == b.id && a.name == b.name && a.connected == b.connected && a.tip == b.tip &&
           a.error == b.error;
}

auto same(const backend::ScanProgress &a, const backend::ScanProgress &b) -> bool {
    return a.id == b.id && a.name == b.name && a.height == b.height && a.target == b.target;
}

auto same(const backend::Endpoint &a, const backend::Endpoint &b) -> bool {
    return a.id == b.id && a.name == b.name && a.url == b.url &&
           a.verifyCertificate == b.verifyCertificate;
}

auto same(const backend::BlockchainState &a, const backend::BlockchainState &b) -> bool {
    return a.scanning == b.scanning && same(a.progress, b.progress) &&
           same(a.connections, b.connections) && a.tip == b.tip && a.error == b.error;
}

auto same(const backend::Settings &a, const backend::Settings &b) -> bool {
    return a.network == b.network && a.networkEditable == b.networkEditable &&
           same(a.endpoints, b.endpoints);
}

} // namespace account
