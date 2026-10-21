#include "PluginAccess.h"

namespace pluginaccess {

auto answers(const QString &plugin_id, const QString &hash) -> PluginAccess {
    return ::app_plugin_answers(rust::String(plugin_id.toStdString()),
                                rust::String(hash.toStdString()));
}

auto stored(const QString &hash) -> plugin::Access {
    auto grants = ::app_plugin_grants(rust::String(hash.toStdString()));
    return {.account = grants.account, .keys = grants.keys, .state = grants.state};
}

auto forPlugin(const QString &plugin_id) -> QList<PluginAccess> {
    QList<PluginAccess> entries;
    for (const auto &entry : ::app_plugin_history(rust::String(plugin_id.toStdString()))) {
        entries.append(entry);
    }
    return entries;
}

} // namespace pluginaccess
