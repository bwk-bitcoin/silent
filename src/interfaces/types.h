#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <interfaces/enums.h>
#include <optional>

using ReqId = quint64;

class IModule;
class ISigner;

namespace plugin {

struct SpendLock {
    SpendLockKind kind = SpendLockKind::None;
    quint32 value{};
};

struct SpendVariant {
    QString pathId;
    quint32 keychain{};
    quint32 derivationIndex{};
    QString display;
    // PSBTv2, one input and no outputs. Merge maps with conflict detection, not combine().
    QString psbtV2Hex;
    QString descriptor;
    // Set instead of descriptor for SP; contains no scan secret.
    QString spSpendKey;
    QList<QString> requiredKeys;
    QList<QString> requiredPreimages;
    // Added WU over empty scriptSig/witness, including witness count; no base input/marker/flag.
    quint64 satisfactionWu{};
    SpendLock absoluteLock;
    SpendLock relativeLock;
    // Timelocks only, not coinbase maturity or signer availability.
    SpendMaturity maturity = SpendMaturity::Unknown;
    // Spendable in the block after the tip: timelocks, coinbase maturity and spent status.
    SpendAvailability availability = SpendAvailability::Unknown;
};

// The validated chain tip a coins() answer was computed against: maturity holds for
// the block after it. Height, hash and MTP come from one view of the chain.
struct ChainTip {
    quint32 height{};
    // Block hash, hex in display order; empty when the account's blockchain backend plugin
    // reports the height alone.
    QString hash;
    // BIP113 median-time-past of the tip; absent when the account's header chain
    // does not hold the 11 blocks it is computed over, or its blockchain backend plugin
    // does not report it.
    std::optional<quint32> medianTimePast;
};

struct Coin {
    QString outpoint;
    quint64 value{};
    quint32 height{};
    QString label;
    bool spent{};
    QString accountType;
    Network network = Network::Bitcoin;
    SpendCoinStatus status = SpendCoinStatus::Unconfirmed;
    std::optional<quint64> confirmationHeight;
    QList<SpendVariant> variants;
};

struct SelectedInput {
    quint32 inputIndex{};
    Network network = Network::Bitcoin;
    SpendVariant variant;
};

struct SigningRequest {
    Network network = Network::Bitcoin;
    SigningStage stage = SigningStage::Signatures;
    QString psbtV2Hex;
    QList<SelectedInput> inputs;
    QString walletName;
    QString fingerprint;
    QString sessionId;
};

struct SigningResult {
    QString psbtV2Hex;
    SigningProgress progress = SigningProgress::Partial;
    QString sessionId;
};

struct Balance {
    quint64 confirmed{};
    quint64 unconfirmed{};
};

// Whether a signer can be used right now. A device the host can see but cannot
// use still gets listed, so the user learns there is something to unlock or fix.
enum class SignerState {
    Ready,       // usable: fingerprint known, requests work
    Locked,      // seen, needs an unlock step (device pairing, PIN)
    Unsupported, // seen, unusable as-is: wrong network, app not open, too old
};

// One live signer. A fingerprint identifies a seed, not a device, so several
// entries can share one: instanceId says which entry this is, module says where
// it came from.
struct SignerEntry {
    QString instanceId; // unique across live instances; the identity
    IModule *module{};  // owning module; meta() names it, synchronously
    ISigner *signer{};  // routing handle for this session
};

// Who makes an IAccount or IKeys request. The host stamps it on every call that goes
// through the handles it gives a module (Host::instances(), Host::keys()).
struct Caller {
    QString moduleId; // the module's meta() "id"
    QString pluginId;
    // Loaded at run time from a plugin library, not linked into Silent.
    bool external{};
    // The sha256 of the plugin library (hex), computed before it was loaded: a plugin
    // update gives a new one. Empty for an internal module.
    QString pluginHash;
};

// What a plugin may use: IAccount (every account), IKeys, and its own storage
// (Host::storage). An internal module may use all three.
struct Access {
    bool account{};
    bool keys{};
    bool state{};
};

} // namespace plugin

Q_DECLARE_METATYPE(plugin::Coin)
Q_DECLARE_METATYPE(plugin::Balance)
Q_DECLARE_METATYPE(QList<plugin::Coin>)
Q_DECLARE_METATYPE(std::optional<plugin::ChainTip>)
Q_DECLARE_METATYPE(std::optional<ReqId>)
Q_DECLARE_METATYPE(plugin::SignError)
Q_DECLARE_METATYPE(plugin::SigningResult)
Q_DECLARE_METATYPE(plugin::RetirementStatus)
Q_DECLARE_METATYPE(plugin::SpendError)
