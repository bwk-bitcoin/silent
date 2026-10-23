#pragma once

#include "interfaces/types.h"

#include <QList>
#include <QString>
#include <cstdint>
#include <silent.h>

// The user's answers to external plugin libraries, kept in the app state by library
// hash.
namespace pluginaccess {

// The answers stored for the library `hash` and the plugin's storage epoch, read when a
// grant request starts. Refused while the plugin's removal is pending.
auto request(const QString &plugin_id, const QString &hash) -> PluginAccessRequest;
// Every answer stored for the plugin `plugin_id`, one entry per library hash, and its
// storage epoch, read when Settings shows them. Refused like `request()`.
auto history(const QString &plugin_id) -> PluginAccessHistory;
// Saves `access` against the epoch its request or history was read with: the error,
// empty once saved.
auto save(const PluginAccess &access, uint64_t expected_epoch) -> QString;
// What the library `hash` may use, by the answers stored: an unasked grant is refused.
auto stored(const QString &hash) -> plugin::Access;
// Every answer stored for the plugin `plugin_id`, one entry per library hash.
auto forPlugin(const QString &plugin_id) -> QList<PluginAccess>;

} // namespace pluginaccess
