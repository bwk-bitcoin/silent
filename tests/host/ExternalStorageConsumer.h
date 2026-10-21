#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <interfaces/storage.h>

class Host;
class IModule;

namespace smoke {

// A storage caller the way a sandboxed plugin is one: it holds only its IStorage and
// exchanges serialized messages with it, each one through a CBOR transport hop. It
// imports no host implementation and never sees a physical path or a native object.
class ExternalStorageConsumer final : public QObject {
    Q_OBJECT

public:
    ExternalStorageConsumer(Host *host, IModule *caller, QString label);

    [[nodiscard]] auto storage() const -> IStorage *;
    // Sends `request` under a fresh id and returns that id.
    auto send(QJsonObject request) -> QString;
    // Sends bytes as they are, for malformed and oversized messages.
    void sendRaw(const QByteArray &message);
    void cancel(const QString &id);
    // The answer to `id`, waiting for it.
    auto wait(const QString &id) -> QJsonObject;
    auto call(const QJsonObject &request) -> QJsonObject;
    // The next answer that carries no id this consumer sent.
    auto waitStray() -> QJsonObject;
    [[nodiscard]] auto answered(const QString &id) const -> bool;

    // Discovers the private storage and opens it, answering its handle.
    auto open() -> QString;
    auto write(const QString &storage, const QString &path, const QByteArray &data,
               const QString &mode) -> QJsonObject;
    auto read(const QString &storage, const QString &path) -> QJsonObject;
    auto inspect(const QString &storage, const QString &path) -> QJsonObject;
    auto mkdir(const QString &storage, const QString &path) -> QJsonObject;
    auto rename(const QString &storage, const QString &path, const QString &to) -> QJsonObject;
    auto remove(const QString &storage, const QString &path, bool recursive) -> QJsonObject;
    auto migrate(const QString &storage, const QString &path) -> QJsonObject;
    // Every entry name of `path`, following `next` page by page of `limit`.
    auto listAll(const QString &storage, const QString &path, int limit) -> QStringList;

    // The whole logical operation matrix on the private storage, below `root`.
    void runContract(const QString &root);

    [[nodiscard]] static auto data(const QJsonObject &answer) -> QByteArray;
    [[nodiscard]] static auto error(const QJsonObject &answer) -> QString;

public slots:
    void onCompleted(const QByteArray &message);

protected:
    void contractFiles(const QString &storage, const QString &root);
    void contractRename(const QString &storage, const QString &root);
    void contractRemove(const QString &storage, const QString &root);
    void contractListing(const QString &storage, const QString &root);
    void contractRefusals(const QString &storage, const QString &root);

private:
    QPointer<IStorage> m_storage;
    QString m_label;
    int m_next = 0;
    QSet<QString> m_sent;
    QHash<QString, QJsonObject> m_answers;
    QList<QJsonObject> m_strays;
};

} // namespace smoke
