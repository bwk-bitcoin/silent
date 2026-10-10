#pragma once

#include <optional>

#include <QString>

#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Badge;
} // namespace catalog

namespace keys {

// What the Keys tab and its modals share.

// The live device holding `fingerprint` that is ready to sign, if one is plugged in.
auto readyDevice(const QString &fingerprint) -> std::optional<plugin::SignerEntry>;

// How a key can be reached right now: a ready device, else its hot secret, else
// neither.
auto keyStatusPill(const KeyInfo &info) -> catalog::Badge *;

auto networkLabel(Network network) -> QString;

} // namespace keys
