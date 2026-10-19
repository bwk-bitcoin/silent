#include "PluginAccess.h"

#include <string>

namespace pluginaccess {

namespace {
auto toQString(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}
} // namespace

auto answers(const QString &plugin_id, const QString &hash) -> PluginAccess {
    QString lowered = hash.toLower();
    for (const auto &entry : ::app_plugin_access()) {
        if (toQString(entry.hash) == lowered) {
            return entry;
        }
    }
    return PluginAccess{
        .plugin_id = rust::String(plugin_id.toStdString()),
        .hash = rust::String(lowered.toStdString()),
        .account = PluginGrant::Unasked,
        .keys = PluginGrant::Unasked,
    };
}

auto stored(const QString &hash) -> plugin::Access {
    auto entry = answers(QString(), hash);
    return {.account = entry.account == PluginGrant::Allowed,
            .keys = entry.keys == PluginGrant::Allowed};
}

auto forPlugin(const QString &plugin_id) -> QList<PluginAccess> {
    QList<PluginAccess> entries;
    for (const auto &entry : ::app_plugin_access()) {
        if (toQString(entry.plugin_id) == plugin_id) {
            entries.append(entry);
        }
    }
    return entries;
}

} // namespace pluginaccess
