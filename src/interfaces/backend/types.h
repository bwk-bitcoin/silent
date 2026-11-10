#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace backend {

using RequestId = quint64;

enum class Network : quint8 { Bitcoin, Testnet, Testnet4, Signet, Regtest };

struct Output {
    QString address;
    quint64 amount{};
    QString label;
    bool maximum{};
};

struct TransactionTemplate {
    QList<Output> outputs;
    QStringList inputs; // Canonical txid:vout identifiers.

    double feeRate{};
    quint64 fee{}; // Nonzero absolute fee overrides feeRate.
};

struct Completion {
    bool complete{};
    QString error;
};

struct CraftResult {
    TransactionTemplate transaction;
    Completion completion;
};

struct Coin {
    QString outpoint;
    quint64 amount{};
    std::optional<quint64> height;

    QString label;
    QString origin;

    bool spent{};
    bool spendable{};
};

// What a blockchain service watches for a descriptor-based account.
struct AccountWatch {
    // The account's public BIP-380 descriptors, each with its checksum. A multipath one
    // (`<0;1>/*`) stands for each of its branches.
    QStringList descriptors;
    // Height of the first block that can pay the account: scanning starts there. Unset when
    // unknown.
    std::optional<quint64> birthdayHeight;
    // Unix time, in seconds, the account was created at. Unset when unknown.
    std::optional<qint64> birthdayTime;
    // Unused indexes watched past the last used one of each branch.
    quint32 gapLimit{20};
};

// One output paying a script of the watched descriptors, spent or not.
struct OwnedOutput {
    // Canonical txid:vout identifier.
    QString outpoint;
    // In satoshis.
    quint64 amount{};
    // The serialized output script.
    QByteArray scriptPubKey;
    // Index in AccountWatch::descriptors of the descriptor deriving the script.
    quint32 descriptor{};
    // The multipath branch of that descriptor (0 receive, 1 change for `<0;1>/*`), 0 for a
    // single-path one.
    quint32 keychain{};
    // The derivation index of the script in that branch.
    quint32 index{};
    // Height of the block confirming the output, unset while unconfirmed.
    std::optional<quint64> height;
    // The output is from a coinbase transaction.
    bool coinbase{};
    // The txid of the transaction spending the output, empty while it is unspent.
    QString spentBy;
};

// How far one branch of a watched descriptor is used.
struct UsedIndex {
    // Index in AccountWatch::descriptors.
    quint32 descriptor{};
    // The multipath branch, as OwnedOutput::keychain.
    quint32 keychain{};
    // The highest derivation index whose script has a transaction, unset when none has.
    std::optional<quint32> last;
};

struct Balance {
    quint64 confirmed{};
    quint64 unconfirmed{};
    quint64 confirmedCount{};
    quint64 unconfirmedCount{};
};

struct Transaction {
    QString txid;
    QString direction;
    QString status;
    QString label;

    std::optional<quint64> amount;
    std::optional<quint64> fee;
    std::optional<quint64> height;
    std::optional<quint64> timestamp;
};

struct TransactionIo {
    quint32 index{};
    QString outpoint;
    QString address;
    QString label;

    std::optional<quint64> amount;
    std::optional<bool> owned;
};

struct TransactionDetails {
    Transaction transaction;
    QList<TransactionIo> inputs;
    QList<TransactionIo> outputs;

    std::optional<quint64> inputTotal;
    std::optional<quint64> outputTotal;
    std::optional<quint64> virtualSize;
    std::optional<double> feeRate;

    QStringList warnings;
};

struct ScanProgress {
    QString id;
    QString name;

    std::optional<quint64> height;
    std::optional<quint64> target;
};

struct Connection {
    QString id;
    QString name;
    bool connected{};

    std::optional<quint64> tip;
    QString error;
};

struct BlockchainState {
    bool scanning{};
    QList<ScanProgress> progress;
    QList<Connection> connections;

    std::optional<quint64> tip;
    QString error;
};

enum class BroadcastOutcome : quint8 { Accepted, Rejected, Unknown };

struct BroadcastResult {
    QString txid;
    BroadcastOutcome outcome{BroadcastOutcome::Unknown};
    QString error;
    QStringList warnings;
};

enum class LabelKind : quint8 { Address, Outpoint, Transaction };

struct LabelKey {
    LabelKind kind{LabelKind::Transaction};
    QString id;
};

struct Label {
    LabelKey key;
    QString text;
};

struct AddressKind {
    QString id;
    QString name;
    bool reusable{};
};

struct Address {
    QString kind;
    QString address;
};

struct AddressVerification {
    QString descriptor;
    QString path;
};

struct Endpoint {
    QString id;
    QString name;
    QString url;
    bool verifyCertificate{true};
};

struct Settings {
    Network network{Network::Bitcoin};
    bool networkEditable{};
    QList<Endpoint> endpoints;
};

// What a backup of an account holds and what opens it, in the current language: empty when
// the account makes none.
struct BackupDescription {
    // Under the Backup section of the account's Settings.
    QString summary;
    // In the Backup Account modal.
    QString details;
};

struct BackupInfo {
    QString backend;
    QString account;
    std::optional<Network> network;

    // What this backup includes and what recovery still requires.
    QStringList includes;
    QStringList requirements;
    QStringList warnings;
};

} // namespace backend

Q_DECLARE_METATYPE(backend::Network)
Q_DECLARE_METATYPE(backend::TransactionTemplate)
Q_DECLARE_METATYPE(backend::CraftResult)
Q_DECLARE_METATYPE(backend::TransactionDetails)
Q_DECLARE_METATYPE(backend::BroadcastResult)
Q_DECLARE_METATYPE(backend::LabelKey)
Q_DECLARE_METATYPE(backend::Address)
Q_DECLARE_METATYPE(backend::Endpoint)
Q_DECLARE_METATYPE(backend::Settings)
Q_DECLARE_METATYPE(backend::BackupInfo)
