#pragma once

#include <silent.h>

// The account panels lay their rows out again only when what they show changed: a
// rebuild every poll would drop a label being edited.

inline auto operator==(const CoinState &a, const CoinState &b) -> bool {
    return a.confirmed_count == b.confirmed_count && a.confirmed_balance == b.confirmed_balance &&
           a.unconfirmed_count == b.unconfirmed_count &&
           a.unconfirmed_balance == b.unconfirmed_balance;
}

inline auto operator==(const RustTx &a, const RustTx &b) -> bool {
    return a.txid == b.txid && a.direction == b.direction && a.amount == b.amount &&
           a.amount_known == b.amount_known && a.fee == b.fee && a.fee_known == b.fee_known &&
           a.height == b.height && a.timestamp == b.timestamp && a.label == b.label &&
           a.status == b.status;
}

inline auto operator==(const RustCoin &a, const RustCoin &b) -> bool {
    return a.outpoint == b.outpoint && a.value == b.value && a.height == b.height &&
           a.timestamp == b.timestamp && a.label == b.label && a.spent == b.spent &&
           a.account_type == b.account_type;
}
