#include "PluginAccess.h"

#include <string>

namespace pluginaccess {

auto request(const QString &plugin_id, const QString &hash) -> PluginAccessRequest {
    return ::app_plugin_access_request(rust::String(plugin_id.toStdString()),
                                       rust::String(hash.toStdString()));
}

auto history(const QString &plugin_id) -> PluginAccessHistory {
    return ::app_plugin_access_history(rust::String(plugin_id.toStdString()));
}

auto save(const PluginAccess &access, uint64_t expected_epoch) -> QString {
    return QString::fromStdString(std::string(::app_set_plugin_access(access, expected_epoch)));
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
