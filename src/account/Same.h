#pragma once

#include "interfaces/backend/types.h"

#include <QList>

// Field by field equality of the account UI service types, so a publication only signals
// what changed and a view only lays out again what did.
namespace account {

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

} // namespace account
