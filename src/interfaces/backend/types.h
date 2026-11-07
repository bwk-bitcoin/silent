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
