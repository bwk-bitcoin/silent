#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
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

} // namespace plugin

Q_DECLARE_METATYPE(plugin::Coin)
Q_DECLARE_METATYPE(plugin::Balance)
Q_DECLARE_METATYPE(QList<plugin::Coin>)
Q_DECLARE_METATYPE(std::optional<ReqId>)
