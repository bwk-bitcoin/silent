#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <optional>

using ReqId = quint64;

class IModule;
class ISigner;

namespace plugin {

struct Coin {
    QString outpoint;
    quint64 value{};
    quint32 height{};
    QString label;
    bool spent{};
    QString accountType;
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

// What a plugin may use: IAccount (every account), IKeys, and its own saved state
// (Host::loadState / saveState). An internal module may use all three.
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
