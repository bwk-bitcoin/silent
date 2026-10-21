#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <optional>
#include <silent.h>

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
    SpendAvailability availability = SpendAvailability::Unknown;
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

enum class RetirementStatus { Pending, Retired, Failed };

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

// Why a sign request failed, beyond its message.
enum class SignError : std::uint8_t {
    Other,
    MusigSession, // the device has no matching MuSig2 session: restart from round 1
};

} // namespace plugin

Q_DECLARE_METATYPE(plugin::Coin)
Q_DECLARE_METATYPE(plugin::Balance)
Q_DECLARE_METATYPE(QList<plugin::Coin>)
Q_DECLARE_METATYPE(std::optional<ReqId>)
Q_DECLARE_METATYPE(plugin::SignError)
Q_DECLARE_METATYPE(plugin::SigningResult)
Q_DECLARE_METATYPE(plugin::RetirementStatus)
Q_DECLARE_METATYPE(SpendError)
