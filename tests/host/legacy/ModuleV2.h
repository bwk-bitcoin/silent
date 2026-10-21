#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

// Frozen IPlugin declarations from de26556, independent of the current SDK.
class IModule;

class IPlugin {
public:
    virtual ~IPlugin() = default;
    [[nodiscard]] virtual QMap<QString, QString> meta() const = 0;
    [[nodiscard]] virtual QList<IModule *> modules() const = 0;
};

#ifdef SILENT_ABI5_FIXTURE
#define SILENT_IPlugin_IID "dev.silent.IPlugin/5.0"
#elif defined(SILENT_ABI4_FIXTURE)
#define SILENT_IPlugin_IID "dev.silent.IPlugin/4.0"
#elif defined(SILENT_ABI3_FIXTURE)
#define SILENT_IPlugin_IID "dev.silent.IPlugin/3.0"
#else
#define SILENT_IPlugin_IID "dev.silent.IPlugin/2.0"
#endif
Q_DECLARE_INTERFACE(IPlugin, SILENT_IPlugin_IID)
