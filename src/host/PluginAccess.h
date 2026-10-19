#pragma once

#include "interfaces/types.h"

#include <QList>
#include <QString>
#include <silent.h>

// The user's answers to external plugin libraries, kept in the app state by library
// hash.
namespace pluginaccess {

// The answers stored for the library `hash`, every grant unasked when none are.
auto answers(const QString &plugin_id, const QString &hash) -> PluginAccess;
// What the library `hash` may use, by the answers stored: an unasked grant is refused.
auto stored(const QString &hash) -> plugin::Access;
// Every answer stored for the plugin `plugin_id`, one entry per library hash.
auto forPlugin(const QString &plugin_id) -> QList<PluginAccess>;

} // namespace pluginaccess
