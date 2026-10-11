#pragma once

#include <optional>

#include <QString>

#include <interfaces/types.h>

namespace keys::flow {

auto sameFingerprint(const QString &left, const QString &right) -> bool;
// The live device signer for `fingerprint`, and its host instance id.
auto liveDevice(const QString &fingerprint) -> std::optional<plugin::SignerEntry>;
auto liveDeviceById(const QString &instance_id) -> std::optional<plugin::SignerEntry>;

} // namespace keys::flow
