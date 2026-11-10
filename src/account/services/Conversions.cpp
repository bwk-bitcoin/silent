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

auto toRustNetwork(backend::Network network) -> std::optional<Network> {
    switch (network) {
    case backend::Network::Bitcoin:
        return Network::Bitcoin;
    case backend::Network::Testnet:
        return Network::Testnet;
    case backend::Network::Signet:
        return Network::Signet;
    case backend::Network::Regtest:
        return Network::Regtest;
    case backend::Network::Testnet4:
        return std::nullopt;
    }
    return std::nullopt;
}

auto toQStrings(const rust::Vec<rust::String> &values) -> QStringList {
    QStringList strings;
    for (const auto &value : values) {
        strings.append(toQString(value));
    }
    return strings;
}

auto toRustStrings(const QStringList &values) -> rust::Vec<rust::String> {
    rust::Vec<rust::String> strings;
    for (const auto &value : values) {
        strings.push_back(rust::String(value.toStdString()));
    }
    return strings;
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

auto toExtras(const RustTxDetails &details) -> QJsonObject {
    QJsonObject scripts;
    for (const auto &output : details.outputs) {
        if (output.address.empty()) {
            scripts.insert(QString::number(output.index), toQString(output.script));
        }
    }
    QJsonObject extras{{QStringLiteral("version"), details.version},
                       {QStringLiteral("lockTime"), static_cast<qint64>(details.lock_time)},
                       {QStringLiteral("scripts"), scripts}};
    if (details.chain_tip_known) {
        extras.insert(QStringLiteral("chainTip"), static_cast<qint64>(details.chain_tip));
    }
    return extras;
}

auto toCapabilities(const BackendInfo &info) -> QJsonObject {
    return {{QStringLiteral("tweaksOnly"), info.tweaks_only},
            {QStringLiteral("tweaksFullBasic"), info.tweaks_full_basic},
            {QStringLiteral("tweaksFullWithDustFilter"), info.tweaks_full_with_dust_filter},
            {QStringLiteral("tweaksCutThroughWithDustFilter"),
             info.tweaks_cut_through_with_dust_filter}};
}

auto endpointPresentation() -> QJsonObject {
    return {{QStringLiteral("blindbit"),
             QJsonObject{{QStringLiteral("label"), TR("settings-blindbit-address")},
                         {QStringLiteral("placeholder"), TR("settings-placeholder-blindbit")},
                         {QStringLiteral("connect"), TR("btn-connect-blindbit")},
                         {QStringLiteral("disconnect"), TR("btn-disconnect-blindbit")},
                         {QStringLiteral("capabilities"), true}}},
            {QStringLiteral("electrum"),
             QJsonObject{{QStringLiteral("label"), TR("settings-electrum-address")},
                         {QStringLiteral("placeholder"), TR("settings-placeholder-electrum")},
                         {QStringLiteral("connect"), TR("btn-connect-electrum")},
                         {QStringLiteral("disconnect"), TR("btn-disconnect-electrum")},
                         {QStringLiteral("transport"), true}}}};
}

auto addressKindTitles() -> QJsonObject {
    return {{QStringLiteral("segwit"), TR("receive-awaiting-segwit")},
            {QStringLiteral("taproot"), TR("receive-awaiting-taproot")}};
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

auto fromTemplate(const TransactionTemplate &transaction) -> backend::TransactionTemplate {
    backend::TransactionTemplate result{.feeRate = transaction.fee_rate, .fee = transaction.fee};
    for (const auto &output : transaction.outputs) {
        result.outputs.append({.address = toQString(output.address),
                               .amount = output.amount,
                               .label = toQString(output.label),
                               .maximum = output.max});
    }
    for (const auto &input : transaction.input_outpoints) {
        result.inputs.append(toQString(input));
    }
    return result;
}

auto toCraftResult(const CraftOutcome &outcome) -> backend::CraftResult {
    return {.transaction = fromTemplate(outcome.transaction),
            .completion = {.complete = outcome.complete, .error = toQString(outcome.error)}};
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

auto toRustString(const QByteArray &bytes) -> rust::String {
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

auto toBackupInfo(const BackupContents &contents) -> backend::BackupInfo {
    bool multikey = toQString(contents.plugin_id) == QStringLiteral("musig");
    backend::BackupInfo info{.backend = multikey ? TR("musig-account-type") : TR("sp-account-type"),
                             .account = toQString(contents.account)};
    if (contents.network_known) {
        info.network = toNetwork(contents.network);
    }
    info.includes.append(multikey ? TR("backup-includes-policy")
                                  : TR("backup-includes-sp-descriptor"));
    for (const auto &endpoint : contents.endpoints) {
        info.includes.append(TR("backup-includes-endpoint").arg(toQString(endpoint)));
    }
    for (const auto &tip : contents.tips) {
        auto kind = toQString(tip.kind) == QStringLiteral("segwit") ? TR("backup-kind-segwit")
                                                                    : TR("backup-kind-taproot");
        info.includes.append(TR("backup-includes-tip").arg(kind).arg(tip.receive).arg(tip.change));
    }
    if (contents.scan_start != 0) {
        info.includes.append(TR("backup-includes-scan-start").arg(contents.scan_start));
    }
    info.includes.append(TR("backup-includes-labels").arg(contents.labels));
    QStringList signers;
    for (const auto &signer : contents.signers) {
        signers.append(toQString(signer));
    }
    // An SP backup holds the scan key, never the spending key.
    if (signers.isEmpty()) {
        info.requirements.append(multikey ? TR("backup-requires-key")
                                          : TR("backup-requires-sp-key"));
    } else {
        auto names = signers.join(QStringLiteral(", "));
        info.requirements.append(multikey ? TR("backup-requires-keys").arg(names)
                                          : TR("backup-requires-sp-keys").arg(names));
    }
    info.requirements.append(contents.scan_start != 0
                                 ? TR("backup-requires-sp-scan").arg(contents.scan_start)
                                 : TR("backup-requires-scan"));
    if (contents.endpoints.empty()) {
        info.requirements.append(TR("backup-requires-servers"));
    }
    if (contents.saved_psbts_excluded) {
        info.warnings.append(TR("backup-warning-saved-psbts"));
    }
    return info;
}

auto backupError(BackupFailure failure, const rust::String &error) -> QString {
    switch (failure) {
    case BackupFailure::Unknown:
        return TR("restore-unknown-format");
    case BackupFailure::Malformed:
        return TR("backup-malformed").arg(toQString(error));
    case BackupFailure::Locked:
        return TR("restore-locked");
    case BackupFailure::InvalidXpub:
        return TR("restore-invalid-xpub");
    case BackupFailure::InvalidMnemonic:
        return TR("restore-invalid-mnemonic");
    case BackupFailure::UnsupportedContent:
        return TR("restore-invalid-type");
    case BackupFailure::InvalidDescriptor:
        return TR("restore-invalid-descriptor");
    case BackupFailure::None:
    case BackupFailure::Unsupported:
    case BackupFailure::Exists:
    case BackupFailure::Other:
        return toQString(error);
    }
    return toQString(error);
}

} // namespace account::services
