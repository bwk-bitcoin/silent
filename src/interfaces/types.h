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
