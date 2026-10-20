#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <silent.h>
#include <string>

namespace smoke {

void pluginAbiSmoke() {
    for (auto legacy : {true, false}) {
        QTemporaryDir directory;
        if (!directory.isValid()) {
            qFatal("ABI fixture directory creation failed");
        }
        auto id = legacy ? QStringLiteral("abi2") : QStringLiteral("abi3");
        auto library = legacy ? QString::fromUtf8(SILENT_ABI2_FIXTURE_PATH)
                              : QString::fromUtf8(SILENT_ABI3_FIXTURE_PATH);
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
                qFatal("registry must reject ABI 2 and accept ABI 3 binaries");
            }
            if (legacy && registry.pluginLoadError(id) != TR("settings-plugin-not-silent")) {
                qFatal("ABI 2 binary must fail at the plugin interface admission gate");
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
