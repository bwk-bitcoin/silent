#pragma once

#include "interfaces/storage.h"

#include <QByteArray>
#include <QList>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QtGlobal>

namespace host {

// One storage request, run off the GUI thread. It holds copies only, never the handle or
// the plugin, so it can outlive both: a revoked or deleted handle just drops its answer.
class StorageJob final : public QThread {
    Q_OBJECT

public:
    StorageJob(quint64 session, QString id, QByteArray message);

    [[nodiscard]] auto id() const -> QString {
        return m_id;
    }

    // The session's grant generation, read before the request ran.
    [[nodiscard]] auto generation() const -> quint64 {
        return m_generation;
    }

    [[nodiscard]] auto result() const -> QByteArray {
        return m_result;
    }

protected:
    void run() override;

private:
    quint64 m_session;
    QString m_id;
    QByteArray m_message;
    quint64 m_generation = 0;
    QByteArray m_result;
};

// The IStorage a module gets from Host::storage(): bound to one Rust storage session
// opened with the caller's registry identity. It runs one request at a time and queues
// the others, MAX_OUTSTANDING in all, the running one counted until its worker ends,
// even once cancelled.
class StorageHandle final : public IStorage {
    Q_OBJECT

public:
    explicit StorageHandle(quint64 session);
    ~StorageHandle() override;

    void request(const QByteArray &message) override;
    void cancel(const QString &id) override;
    // Closes the session: outstanding requests get no answer, later ones answer revoked.
    void revoke();

    [[nodiscard]] auto drained() const -> bool {
        return m_job == nullptr;
    }

public slots:
    void onJobFinished();

protected:
    // Answers `id` with `error` now, through completed().
    void refuse(const QString &id, const QString &error);
    [[nodiscard]] auto outstanding(const QString &id) const -> bool;
    void dispatch();

private:
    struct Queued {
        QString id;
        QByteArray message;
    };

    quint64 m_session = 0;
    bool m_revoked = false;
    QList<Queued> m_queue;
    QPointer<StorageJob> m_job;
    // The running request was cancelled: it was answered, its result is dropped.
    bool m_job_cancelled = false;
};

} // namespace host
