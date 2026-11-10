#pragma once

#include "interfaces/backend/types.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>
#include <silent.h>

// Conversions between the Rust bridge types and the account UI service types. Unknown
// values (a zero height, an unknown amount) become empty optionals.
namespace account::services {

auto toQString(const rust::String &value) -> QString;
auto toNetwork(Network network) -> backend::Network;
// The bridge network of `network`, none for a network Silent accounts are not on.
auto toRustNetwork(backend::Network network) -> std::optional<Network>;
auto toQStrings(const rust::Vec<rust::String> &values) -> QStringList;
auto toRustStrings(const QStringList &values) -> rust::Vec<rust::String>;
// `spendable`: a spend path can spend the coin now (its timelock, if any, is met).
auto toCoin(const RustCoin &coin, bool spendable) -> backend::Coin;
auto toBalance(const CoinState &state) -> backend::Balance;
auto toTransaction(const RustTx &tx) -> backend::Transaction;
auto toDetails(const RustTxDetails &details) -> backend::TransactionDetails;
// What toDetails() leaves out, as extension::TRANSACTION_EXTRAS answers it.
auto toExtras(const RustTxDetails &details) -> QJsonObject;
// A BlindBit server's capabilities, as extension::ENDPOINT_CAPABILITIES answers them.
auto toCapabilities(const BackendInfo &info) -> QJsonObject;
// How Settings shows the built-in endpoints, as extension::ENDPOINT_PRESENTATION answers.
auto endpointPresentation() -> QJsonObject;
// The built-in address kinds' awaiting titles, as extension::ADDRESS_KIND_TITLES answers.
auto addressKindTitles() -> QJsonObject;
auto toLabel(const LabelEntry &entry) -> backend::Label;
auto toLabelKind(backend::LabelKind kind) -> LabelKind;
auto toAddressKind(const ReceiveKind &kind) -> backend::AddressKind;
auto toAddress(const ReceiveAddress &address) -> backend::Address;
auto toVerification(const ReceiveAddress &address) -> backend::AddressVerification;
auto toTemplate(const backend::TransactionTemplate &transaction) -> TransactionTemplate;
auto fromTemplate(const TransactionTemplate &transaction) -> backend::TransactionTemplate;
auto toCraftResult(const CraftOutcome &outcome) -> backend::CraftResult;
auto toBroadcastResult(const BroadcastReport &report) -> backend::BroadcastResult;
auto toBytes(const rust::Vec<std::uint8_t> &bytes) -> QByteArray;
auto toSlice(const QByteArray &bytes) -> rust::Slice<const std::uint8_t>;
// `bytes` as UTF-8 text, a secret.
auto toRustString(const QByteArray &bytes) -> rust::String;
// What an SP account's backup holds and what opens it, said in the current language.
auto spBackupDescription() -> backend::BackupDescription;
// The same for a Multikey account.
auto multikeyBackupDescription() -> backend::BackupDescription;
// What a backup holds and needs, said in the current language.
auto toBackupInfo(const BackupContents &contents) -> backend::BackupInfo;
// Why a backup was not made, read or restored, said in the current language.
auto backupError(BackupFailure failure, const rust::String &error) -> QString;

} // namespace account::services
