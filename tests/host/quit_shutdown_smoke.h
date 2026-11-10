#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <interfaces/module.h>
#include <interfaces/storage.h>

namespace smoke {

// A module that saves a file while it shuts down and answers shutdownFinished once that
// write is answered. A stuck one never answers.
class QuitModule final : public IModule {
    Q_OBJECT

public:
    QuitModule(QString id, bool stuck, QObject *parent = nullptr);

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override;
    auto list() -> ReqId override;
    auto autoStart() -> ReqId override;
    void startInstance(const QString &id) override;
    void shutdown(ReqId req_id) override;

    // Writes go to `path`/shutdown.txt of the storage `handle`.
    void prepare(IStorage *storage, const QString &handle, const QString &path);
    [[nodiscard]] auto shutdownCalls() const -> int;
    [[nodiscard]] auto answers() const -> const QHash<QString, QJsonObject> &;

public slots:
    void onCompleted(const QByteArray &message);

protected:
    void write(const QString &id, const QString &name);

private:
    QString m_id;
    bool m_stuck = false;
    QPointer<IStorage> m_storage;
    QString m_handle;
    QString m_path;
    int m_shutdown_calls = 0;
    ReqId m_shutdown_req = 0;
    ReqId m_next = 0;
    QHash<QString, QJsonObject> m_answers;
};

class QuitPlugin final : public QObject, public IPlugin {
    Q_OBJECT
    Q_INTERFACES(IPlugin)

public:
    QuitPlugin();

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override;
    [[nodiscard]] auto modules() const -> QList<IModule *> override;
    [[nodiscard]] auto saver() const -> QuitModule *;
    [[nodiscard]] auto stuck() const -> QuitModule *;

private:
    QuitModule *m_saver = nullptr;
    QuitModule *m_stuck = nullptr;
};

// ws-plaude/silent#102: the quit path of main() shuts every module down, within its bound.
void quitShutdownSmoke();

} // namespace smoke
