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
    // All async work belongs to these modules; constructors stay idle until activation.
    [[nodiscard]] virtual QList<IModule *> modules() const = 0;
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

    // Runtime release only. Tab close and plugin unload must preserve saved data.
    virtual void deleteInstance(const QString & /*id*/) {
    }

    // Explicit confirmed deletion. Record the nonzero request ID before dispatch:
    // refusal may answer inside this call. Empty error means shutdown, backend
    // destruction and authorized storage removal all completed successfully.
    virtual void deleteAccount(ReqId req_id, const QString &id) {
        emit accountDeleted(req_id, id, QStringLiteral("Account deletion is not supported"));
    }

    // Stop accepting work and asynchronously drain every owned instance, including
    // already-closing ones, workers, callbacks and backend writes. Success promises
    // no owned work or callbacks can outlive plugin destruction. Never infer this from a
    // removed host registration. Refusal keeps the library mapped and retryable.
    virtual void shutdown(ReqId req_id) {
        emit shutdownFinished(req_id, QStringLiteral("Module shutdown is not supported"));
    }

    // The host tells every module whether signing devices must be polled; it is on
    // while any module has asked for it through Host::requestSignerPolling. A module
    // that owns a device service starts or stops it here. Default: no-op for modules
    // that poll nothing.
    virtual void setSignerPolling(bool /*enabled*/) {
    }

signals:
    void shutdownFinished(ReqId req_id, QString error);
    void accountDeleted(ReqId req_id, QString id, QString error);
    void instances(ReqId req_id, QList<QPair<QString, QString>> instances);
    void autoStart(ReqId req_id, QStringList ids);
    void error(std::optional<ReqId> req_id, QString message);
};

#define SILENT_IPlugin_IID "dev.silent.IPlugin/1.0"
Q_DECLARE_INTERFACE(IPlugin, SILENT_IPlugin_IID)

#define SILENT_IModule_IID "dev.silent.IModule/1.0"
