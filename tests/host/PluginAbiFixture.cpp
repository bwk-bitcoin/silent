#if defined(SILENT_ABI2_FIXTURE) || defined(SILENT_ABI3_FIXTURE) ||                                \
    defined(SILENT_ABI4_FIXTURE) || defined(SILENT_ABI5_FIXTURE) || defined(SILENT_ABI6_FIXTURE)
#include "legacy/ModuleV2.h"
#else
#include "interfaces/module.h"
#endif

#include <cstdio>
#include <cstdlib>

namespace {
void recordSentinel(const char *event) {
    auto *path = std::getenv("SILENT_ABI_SENTINEL");
    if (path == nullptr) {
        return;
    }
    auto *file = std::fopen(path, "ab");
    if (file == nullptr || std::fputs(event, file) < 0) {
        qFatal("ABI fixture sentinel write failed");
    }
    if (std::fclose(file) != 0) {
        qFatal("ABI fixture sentinel close failed");
    }
}

class LibraryInitializer {
public:
    LibraryInitializer() {
        recordSentinel("initializer\n");
    }
};

LibraryInitializer initializer;
} // namespace

class Q_DECL_HIDDEN PluginAbiFixture final : public QObject, public IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SILENT_IPlugin_IID)
    Q_INTERFACES(IPlugin)

public:
    PluginAbiFixture() {
        recordSentinel("constructor\n");
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
#if defined(SILENT_ABI2_FIXTURE) || defined(SILENT_ABI3_FIXTURE) ||                                \
    defined(SILENT_ABI4_FIXTURE) || defined(SILENT_ABI5_FIXTURE) || defined(SILENT_ABI6_FIXTURE)
        qFatal("registry must reject old ABI before calling plugin virtual methods");
#else
        return {{QStringLiteral("id"), QStringLiteral("abi7")}};
#endif
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
#if defined(SILENT_ABI2_FIXTURE) || defined(SILENT_ABI3_FIXTURE) ||                                \
    defined(SILENT_ABI4_FIXTURE) || defined(SILENT_ABI5_FIXTURE) || defined(SILENT_ABI6_FIXTURE)
        qFatal("registry must reject old ABI before discovering modules or feeds");
#else
        return {};
#endif
    }
};

#include "PluginAbiFixture.moc"
