#pragma once

#include "interfaces/module.h"

#include <QHash>
#include <QPointer>
#include <QString>
#include <silent.h>

class AccountBackend;

namespace account {

// Shared built-in lifecycle coordinator, not a required external plugin base.
class AccountModule : public IModule {
    Q_OBJECT

public:
    using Operation = rust::String (*)(rust::String);
    AccountModule(Operation check, Operation remove);
    void deleteAccount(ReqId req_id, const QString &id) override;
    [[nodiscard]] auto accountUnavailable(const QString &id) const -> bool;
    void watchAccount(const QString &id, AccountBackend *backend);

signals:
    void deletionChecked(QString id, QString error);
    void deletionRemoved(QString id, QString error);

public slots:
    void onShutdownFinished(const QString &error);
    void onDeletionChecked(const QString &id, const QString &error);
    void onDeletionRemoved(const QString &id, const QString &error);

protected:
    void finishDeletion(const QString &id, const QString &error);
    void removeAccountStorage(const QString &id);

private:
    struct Runtime {
        QPointer<AccountBackend> backend;
        bool stopped = false;
        QString error;
    };

    QHash<QString, Runtime> m_runtimes;

    struct Deletion {
        ReqId req_id = 0;
        bool validated = false;
        bool removing = false;
    };

    QHash<QString, Deletion> m_pending;
    Operation m_check;
    Operation m_remove;
};

} // namespace account
