#include "LiveDevice.h"

#include "interfaces/host.h"
#include "interfaces/signer.h"

namespace keys::flow {

auto sameFingerprint(const QString &left, const QString &right) -> bool {
    return QString::compare(left, right, Qt::CaseInsensitive) == 0;
}

auto liveDevice(const QString &fingerprint) -> std::optional<plugin::SignerEntry> {
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && sameFingerprint(entry.signer->fingerprint(), fingerprint)) {
            return entry;
        }
    }
    return std::nullopt;
}

auto liveDeviceById(const QString &instance_id) -> std::optional<plugin::SignerEntry> {
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.instanceId == instance_id) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace keys::flow
