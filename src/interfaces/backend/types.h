#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace backend {

// Pairs a request with its answer: the caller picks it, unique on the service it calls (the
// built-in views take account::nextRequestId(), never 0), and the answering signal or error()
// carries it back. 0 names no request: error(0, ...) reports a failure of a call that takes
// none (ITransaction::finalize()).
using RequestId = quint64;

// The Bitcoin network an account is on. No built-in account is on Testnet4.
enum class Network : quint8 {
    // Mainnet.
    Bitcoin,
    // Testnet3.
    Testnet,
    // Testnet4 (BIP94).
    Testnet4,
    // The default signet (BIP325).
    Signet,
    // A local regression test network.
    Regtest
};

// One recipient of a transaction template.
struct Output {
    // The recipient address (an SP address, any address of the account's network), or the hex
    // data of an OP_RETURN output.
    QString address;
    // In satoshis, 0 for an OP_RETURN output. Ignored while `maximum` is set: a complete
    // CraftResult states the amount resolved.
    quint64 amount{};
    // The label the user gave the recipient, empty for none. Carried with the template: the
    // built-in accounts save it nowhere.
    QString label;
    // Sends what is left once the other outputs and the fee are paid. At most one output.
    bool maximum{};
};

// A transaction to build: its recipients, its inputs and its fee.
struct TransactionTemplate {
    // The recipients, in the order of the transaction's outputs; change is not among them.
    QList<Output> outputs;
    // Canonical txid:vout identifiers of the coins spent, empty while none is picked: such a
    // template stays incomplete until ITransaction::selectCoins() picks them.
    QStringList inputs;

    // In sat/vB, used while `fee` is 0.
    double feeRate{};
    // In satoshis. Nonzero absolute fee overrides feeRate.
    quint64 fee{};
};

// Whether a template can be built as it is.
struct Completion {
    // Buildable as it is: inputs given, funds enough, recipients valid.
    bool complete{};
    // Why it is not complete, in words for the user; empty when complete.
    QString error;
};

// A template checked by ITransaction.
struct CraftResult {
    // The template checked; once complete, the whole transaction: its inputs, the `maximum`
    // amount resolved and the absolute fee it pays in `fee`.
    TransactionTemplate transaction;
    // Whether `transaction` can be built as it is, and why not.
    Completion completion;
};

// One output the account owns, spent or not.
struct Coin {
    // Canonical txid:vout identifier.
    QString outpoint;
    // In satoshis.
    quint64 amount{};
    // Height of the block confirming the coin, unset while unconfirmed.
    std::optional<quint64> height;

    // The coin's label (an Outpoint label), empty for none.
    QString label;
    // The kind of script the coin pays, shown as it is: "SP", "Segwit" or "Taproot" in the
    // built-in accounts ("Unknown" for a sub-account of none of them).
    QString origin;

    // Spent, or spent by a transaction not yet confirmed.
    bool spent{};
    // A spend path can spend it now: unspent, and a timelocked path only once the validated
    // tip meets its lock.
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

// The account's unspent coins, in satoshis, and how many there are.
struct Balance {
    // Of the confirmed coins.
    quint64 confirmed{};
    // Of the coins not yet confirmed.
    quint64 unconfirmed{};
    // How many unspent coins `confirmed` sums: those with a block height at the account's
    // last scanned tip.
    quint64 confirmedCount{};
    // How many unspent coins `unconfirmed` sums: those with no block height yet.
    quint64 unconfirmedCount{};
};

// One transaction of the account's history.
struct Transaction {
    // Hex txid.
    QString txid;
    // "incoming", "outgoing" or "self" (it pays the account only); anything else, empty
    // included, is unknown.
    QString direction;
    // "unconfirmed", "confirmed-unverified", "verified" (its merkle proof checked),
    // "verify-failed", "broadcast" or "finalized" (a built-in Multikey transaction it signed,
    // sent or not, its scan has not seen yet) or "unknown".
    QString status;
    // The transaction's label, empty for none.
    QString label;

    // In satoshis, what the transaction moves in or out of the account, `direction` saying
    // which. Unset when unknown.
    std::optional<quint64> amount;
    // In satoshis, unset when unknown.
    std::optional<quint64> fee;
    // Height of the confirming block, unset while unconfirmed.
    std::optional<quint64> height;
    // Unix time, in seconds, of the confirming block, unset while unconfirmed or unknown.
    std::optional<quint64> timestamp;
};

// One input or output of a transaction.
struct TransactionIo {
    // Its position among the transaction's inputs or outputs.
    quint32 index{};
    // Canonical txid:vout identifier: the output an input spends, or the output itself.
    QString outpoint;
    // The address the output pays, empty when its script pays none (OP_RETURN) or unknown.
    QString address;
    // The label of `outpoint`, empty for none.
    QString label;

    // In satoshis, unset when unknown (an input whose funding transaction is not fetched).
    std::optional<quint64> amount;
    // It pays one of the account's scripts, unset when unknown.
    std::optional<bool> owned;
};

// What the account knows of one transaction, fetched by IBlockchain::fetchTransactionDetails().
struct TransactionDetails {
    // Its history entry, its fee as the details compute it; "unknown" status when it is none
    // of the history's.
    Transaction transaction;
    // In transaction order.
    QList<TransactionIo> inputs;
    QList<TransactionIo> outputs;

    // In satoshis, unset when an input amount is unknown.
    std::optional<quint64> inputTotal;
    // In satoshis, unset when unknown.
    std::optional<quint64> outputTotal;
    // In virtual bytes, unset when unknown.
    std::optional<quint64> virtualSize;
    // In sat/vB, unset while the fee is unknown.
    std::optional<double> feeRate;

    // In words for the user: what is missing from these details (an input amount, a lookup
    // done offline). Empty when nothing is.
    QStringList warnings;
};

// How far one scan of the chain has gone.
struct ScanProgress {
    // The id of the Connection the scan runs on.
    QString id;
    // Its name, as Connection::name.
    QString name;

    // The last block height scanned, unset when unknown.
    std::optional<quint64> height;
    // The block height the scan goes up to, unset when unknown.
    std::optional<quint64> target;
};

// One server the blockchain service syncs from, as IBlockchain::state() lists it (the
// built-in accounts' BlindBit and Electrum servers).
struct Connection {
    // Stable, the same as the Endpoint::id of this server in IBackendSettings::settings():
    // what setConnected() and refreshTip() take. "blindbit" and "electrum" in the built-in
    // accounts.
    QString id;
    // What the user is shown, in the current language.
    QString name;
    // The service is connected to the server.
    bool connected{};

    // The chain height the server answered to the last refreshTip(), unset until one is
    // answered.
    std::optional<quint64> tip;
    // In words for the user: why the last attempt to connect to or use the server failed,
    // empty while none did.
    QString error;
};

// The state of the blockchain service: its scans and its connections.
struct BlockchainState {
    // A scan runs and has not reached its target.
    bool scanning{};
    // One entry per scan, empty when the service reports none.
    QList<ScanProgress> progress;
    // One entry per server, empty when the service has none.
    QList<Connection> connections;

    // The chain height the service syncs up to, unset when unknown. A built-in account takes
    // it from its BlindBit scan's target, else from the Electrum tip last refreshed.
    std::optional<quint64> tip;
    // In words for the user: why the service cannot sync at all (the account did not load),
    // empty when nothing stops it.
    QString error;
};

// The outcome of IBlockchain::broadcast().
enum class BroadcastOutcome : quint8 {
    // The server took the transaction, or already held it.
    Accepted,
    // The transaction did not reach the network, or the server refused it.
    Rejected,
    // Sent, but whether the server took it is not known.
    Unknown
};

// The answer of IBlockchain::broadcast().
struct BroadcastResult {
    // Hex txid of the transaction, empty when its bytes do not decode.
    QString txid;
    // Whether the server took it.
    BroadcastOutcome outcome{BroadcastOutcome::Unknown};
    // In words for the user: why it is not Accepted, empty when it is.
    QString error;
    // After acceptance: what the account failed to record locally, in words for the user.
    QStringList warnings;
};

// What a label is put on, which sets the form of LabelKey::id.
enum class LabelKind : quint8 {
    // An address.
    Address,
    // A canonical txid:vout outpoint.
    Outpoint,
    // A hex txid.
    Transaction
};

// What a label is put on, as ILabels reads and sets it: one address, outpoint or
// transaction of the account.
struct LabelKey {
    // Which kind of thing `id` names.
    LabelKind kind{LabelKind::Transaction};
    // The address, outpoint or txid `kind` names, in its form.
    QString id;
};

// A label of the account: a short user text put on an address, outpoint or transaction.
struct Label {
    // What it is put on.
    LabelKey key;
    // As ILabels::setLabel() set it: a label set to an empty text is removed.
    QString text;
};

// A kind of address the account receives on.
struct AddressKind {
    // Stable: what IAccountBackend::newAddress() takes and Address::kind holds. "sp",
    // "segwit" or "taproot" in the built-in accounts.
    QString id;
    // What the user is shown, in the current language.
    QString name;
    // One static address, handed out again and again (a silent payment address): it is in
    // IAccountBackend::addresses() from the start. Otherwise newAddress() hands out a new one
    // each time.
    bool reusable{};
};

// A receive address the account handed out.
struct Address {
    // The AddressKind::id of its kind.
    QString kind;
    // The address, encoded for the account's network.
    QString address;
};

// What a signing device needs to show an address and check it is the account's.
struct AddressVerification {
    // The descriptor deriving the address, as the account holds it: its sp() descriptor for
    // a silent payment address, else a BIP-380 one.
    QString descriptor;
    // Where in `descriptor` the address is: "m" for the sp() one, else "<branch>/<index>",
    // the multipath branch (0 receive) then the derivation index.
    QString path;
};

// A server the account can sync from, as Settings edits it.
struct Endpoint {
    // Stable, the same as Connection::id. "blindbit" and "electrum" in the built-in accounts.
    QString id;
    // What the user is shown, in the current language.
    QString name;
    // The server's address, in the form its kind reads (a built-in Electrum one `host:port`,
    // `ssl://` first for TLS; a BlindBit one an http(s) URL). Empty when none is set.
    QString url;
    // The server's TLS certificate is checked. Only an Electrum server over TLS reads it in
    // the built-in accounts.
    bool verifyCertificate{true};
};

// The settings of an account's blockchain service.
struct Settings {
    // The network the account is on.
    Network network{Network::Bitcoin};
    // save() may change `network`. False in the built-in accounts, which refuse it.
    bool networkEditable{};
    // Every server the account syncs from, empty when it has none to set.
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

// What an opened backup holds, as IAccountBackend::inspectBackup() reads it. The texts are in
// the current language.
struct BackupInfo {
    // The kind of account it restores, as the user is shown it.
    QString backend;
    // The account's name the backup names, empty when it names none: restore() is then given
    // one.
    QString account;
    // Unset when neither the backup nor its keys tell the network: restore() is then given
    // one.
    std::optional<Network> network;

    // What this backup includes and what recovery still requires, one line each.
    QStringList includes;
    QStringList requirements;
    // What a restore loses, one line each; empty when nothing.
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
