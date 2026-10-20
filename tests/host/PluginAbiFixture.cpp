#ifdef SILENT_ABI2_FIXTURE
#include "legacy/ModuleV2.h"
#else
#include "interfaces/module.h"
#endif

class Q_DECL_HIDDEN PluginAbiFixture final : public QObject, public IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SILENT_IPlugin_IID)
    Q_INTERFACES(IPlugin)

public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
#ifdef SILENT_ABI2_FIXTURE
        qFatal("registry must reject ABI 2 before calling plugin virtual methods");
#else
        return {};
#endif
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
#ifdef SILENT_ABI2_FIXTURE
        qFatal("registry must reject ABI 2 before discovering modules or feeds");
#else
        return {};
#endif
    }
};

#include "PluginAbiFixture.moc"
