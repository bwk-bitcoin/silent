#pragma once

#include <optional>

#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <interfaces/types.h>

class IModule;

class IPlugin {
public:
    virtual ~IPlugin() = default;

    [[nodiscard]] virtual QMap<QString, QString> meta() const = 0;
    [[nodiscard]] virtual QList<IModule*> modules() const = 0;
};

class IModule : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IModule() override = default;

    [[nodiscard]] virtual QMap<QString, QString> meta() const = 0;
    virtual ReqId list() = 0;
    virtual ReqId autoStart() = 0;
    virtual void startInstance(const QString &id) = 0;
    // Optional teardown hook. The module owns its persistence, so when the host
    // deletes an instance it asks the owning module to drop that instance's
    // on-disk state. The host removes the live instance and its pushed UI itself;
    // modules that persist instances override this to delete their stored state.
    // Default: no-op (nothing persisted).
    virtual void deleteInstance(const QString & /*id*/) {}

    // The host tells every module whether signing devices must be polled; it is on
    // while any module has asked for it through Host::requestSignerPolling. A module
    // that owns a device service starts or stops it here. Default: no-op for modules
    // that poll nothing.
    virtual void setSignerPolling(bool /*enabled*/) {}

signals:
    void instances(ReqId req_id, QList<QPair<QString, QString>> instances);
    void autoStart(ReqId req_id, QStringList ids);
    void error(std::optional<ReqId> req_id, QString message);
};

#define SILENT_IPlugin_IID "dev.silent.IPlugin/1.0"
Q_DECLARE_INTERFACE(IPlugin, SILENT_IPlugin_IID)

#define SILENT_IModule_IID "dev.silent.IModule/1.0"
