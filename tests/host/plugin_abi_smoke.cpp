#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <silent.h>
#include <string>

namespace smoke {

void pluginAbiSmoke() {
    for (auto version : {2, 3, 4}) {
        auto legacy = version != 4;
        QTemporaryDir directory;
        if (!directory.isValid()) {
            qFatal("ABI fixture directory creation failed");
        }
        auto id = QStringLiteral("abi%1").arg(version);
        auto library = QString::fromUtf8(SILENT_ABI4_FIXTURE_PATH);
        if (version == 2) {
            library = QString::fromUtf8(SILENT_ABI2_FIXTURE_PATH);
        } else if (version == 3) {
            library = QString::fromUtf8(SILENT_ABI3_FIXTURE_PATH);
        }
        auto libraryPath = QDir(directory.path()).filePath(QStringLiteral("lib%1.so").arg(id));
        if (!QFile::copy(library, libraryPath)) {
            qFatal("ABI fixture library copy failed");
        }
        QFile metadata(QDir(directory.path()).filePath(id + QStringLiteral(".json")));
        auto json = QStringLiteral("{\"id\":\"%1\",\"name\":\"ABI fixture\"}").arg(id).toUtf8();
        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(json) != json.size()) {
            qFatal("ABI fixture sidecar write failed");
        }
        metadata.close();
        auto installed = ::plugin_install(rust::String(directory.path().toStdString()));
        if (!installed.is_ok) {
            qFatal("ABI fixture install failed: %s", installed.error.c_str());
        }
        {
            PluginRegistry registry;
            registry.rescanDiscoveredPlugins();
            auto enabled = registry.setEnabled(id, true);
            if (enabled == legacy || registry.isPluginEnabled(id) == legacy) {
                qFatal("registry must reject ABI 2/3 and accept ABI 4 binaries");
            }
            if (legacy && registry.pluginLoadError(id) != TR("settings-plugin-not-silent")) {
                qFatal("old ABI binary must fail at the plugin interface admission gate");
            }
            if (!registry.enabledModulesForLauncher().empty()) {
                qFatal("ABI fixture must register no modules or feed instances");
            }
        }
        auto error = ::plugin_remove(rust::String(id.toStdString()));
        if (!error.empty()) {
            qFatal("ABI fixture removal failed: %s", error.c_str());
        }
    }
}

} // namespace smoke
