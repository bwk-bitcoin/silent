#include "account/services/Conversions.h"

#include "i18n/Tr.h"

#include <optional>
#include <string>

namespace account::services {
namespace {

auto known(bool is_known, std::uint64_t value) -> std::optional<quint64> {
    if (!is_known) {
        return std::nullopt;
    }
    return value;
}

// Zero stands for "not yet" in heights and timestamps.
auto nonZero(std::uint64_t value) -> std::optional<quint64> {
    return known(value != 0, value);
}

auto toIo(const RustTxIo &io) -> backend::TransactionIo {
    return {.index = io.index,
            .outpoint = toQString(io.outpoint),
            .address = toQString(io.address),
            .label = toQString(io.label),
            .amount = known(io.value_known, io.value),
            .owned = io.ownership_known ? std::optional<bool>(io.is_mine) : std::nullopt};
}

} // namespace

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

auto toNetwork(Network network) -> backend::Network {
    switch (network) {
    case Network::Bitcoin:
        return backend::Network::Bitcoin;
    case Network::Testnet:
        return backend::Network::Testnet;
    case Network::Signet:
        return backend::Network::Signet;
    case Network::Regtest:
        return backend::Network::Regtest;
    }
    qFatal("Unknown Rust network");
}

auto toCoin(const RustCoin &coin, bool spendable) -> backend::Coin {
    return {.outpoint = toQString(coin.outpoint),
            .amount = coin.value,
            .height = nonZero(coin.height),
            .label = toQString(coin.label),
            .origin = toQString(coin.account_type),
            .spent = coin.spent,
            .spendable = spendable};
}

auto toBalance(const CoinState &state) -> backend::Balance {
    return {.confirmed = state.confirmed_balance,
            .unconfirmed = state.unconfirmed_balance,
            .confirmedCount = state.confirmed_count,
            .unconfirmedCount = state.unconfirmed_count};
}

auto toTransaction(const RustTx &tx) -> backend::Transaction {
    return {.txid = toQString(tx.txid),
            .direction = toQString(tx.direction),
            .status = toQString(tx.status),
            .label = toQString(tx.label),
            .amount = known(tx.amount_known, tx.amount),
            .fee = known(tx.fee_known, tx.fee),
            .height = nonZero(tx.height),
            .timestamp = nonZero(tx.timestamp)};
}

auto toDetails(const RustTxDetails &details) -> backend::TransactionDetails {
    backend::TransactionDetails result{
        .transaction = toTransaction(details.tx),
        .inputTotal = known(details.input_total_known, details.input_total),
        .outputTotal = details.output_total,
        .virtualSize = nonZero(details.vsize),
        .feeRate = details.fee_known ? std::optional<double>(details.fee_rate) : std::nullopt};
    result.transaction.fee = known(details.fee_known, details.fee);
    for (const auto &input : details.inputs) {
        result.inputs.append(toIo(input));
    }
    for (const auto &output : details.outputs) {
        result.outputs.append(toIo(output));
    }
    if (!details.warning.empty()) {
        result.warnings.append(toQString(details.warning));
    }
    return result;
}

auto toLabel(const LabelEntry &entry) -> backend::Label {
    backend::LabelKey key{.id = toQString(entry.id)};
    switch (entry.kind) {
    case LabelKind::Address:
        key.kind = backend::LabelKind::Address;
        break;
    case LabelKind::Outpoint:
        key.kind = backend::LabelKind::Outpoint;
        break;
    case LabelKind::Transaction:
        key.kind = backend::LabelKind::Transaction;
        break;
    }
    return {.key = key, .text = toQString(entry.text)};
}

auto toLabelKind(backend::LabelKind kind) -> LabelKind {
    switch (kind) {
    case backend::LabelKind::Address:
        return LabelKind::Address;
    case backend::LabelKind::Outpoint:
        return LabelKind::Outpoint;
    case backend::LabelKind::Transaction:
        return LabelKind::Transaction;
    }
    qFatal("Unknown label kind");
}

auto toAddressKind(const ReceiveKind &kind) -> backend::AddressKind {
    auto id = toQString(kind.id);
    auto name = id;
    if (id == QStringLiteral("sp")) {
        name = TR("receive-silent-payment-address");
    } else if (id == QStringLiteral("segwit")) {
        name = TR("receive-segwit-address");
    } else if (id == QStringLiteral("taproot")) {
        name = TR("receive-taproot-address");
    }
    return {.id = id, .name = name, .reusable = kind.reusable};
}

auto toAddress(const ReceiveAddress &address) -> backend::Address {
    return {.kind = toQString(address.kind), .address = toQString(address.address)};
}

auto toVerification(const ReceiveAddress &address) -> backend::AddressVerification {
    return {.descriptor = toQString(address.descriptor), .path = toQString(address.path)};
}

auto toTemplate(const backend::TransactionTemplate &transaction) -> TransactionTemplate {
    TransactionTemplate result{};
    for (const auto &output : transaction.outputs) {
        result.outputs.push_back(Output{.address = rust::String(output.address.toStdString()),
                                        .amount = output.amount,
                                        .label = rust::String(output.label.toStdString()),
                                        .max = output.maximum});
    }
    result.fee_rate = transaction.feeRate;
    result.fee = transaction.fee;
    for (const auto &input : transaction.inputs) {
        result.input_outpoints.push_back(rust::String(input.toStdString()));
    }
    return result;
}

auto toCraftResult(const CraftOutcome &outcome) -> backend::CraftResult {
    backend::CraftResult result{
        .transaction = {.feeRate = outcome.transaction.fee_rate, .fee = outcome.transaction.fee},
        .completion = {.complete = outcome.complete, .error = toQString(outcome.error)}};
    for (const auto &output : outcome.transaction.outputs) {
        result.transaction.outputs.append({.address = toQString(output.address),
                                           .amount = output.amount,
                                           .label = toQString(output.label),
                                           .maximum = output.max});
    }
    for (const auto &input : outcome.transaction.input_outpoints) {
        result.transaction.inputs.append(toQString(input));
    }
    return result;
}

auto toBroadcastResult(const BroadcastReport &report) -> backend::BroadcastResult {
    backend::BroadcastResult result{.txid = toQString(report.txid),
                                    .error = toQString(report.error)};
    switch (report.outcome) {
    case BroadcastOutcome::Accepted:
        result.outcome = backend::BroadcastOutcome::Accepted;
        break;
    case BroadcastOutcome::Rejected:
        result.outcome = backend::BroadcastOutcome::Rejected;
        break;
    case BroadcastOutcome::Unknown:
        result.outcome = backend::BroadcastOutcome::Unknown;
        break;
    }
    for (const auto &warning : report.warnings) {
        result.warnings.append(toQString(warning));
    }
    return result;
}

auto toBytes(const rust::Vec<std::uint8_t> &bytes) -> QByteArray {
    return {reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}

auto toSlice(const QByteArray &bytes) -> rust::Slice<const std::uint8_t> {
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            static_cast<std::size_t>(bytes.size())};
}

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

} // namespace account::services
