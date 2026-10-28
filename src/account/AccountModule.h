#pragma once

#include "interfaces/module.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <memory>
#include <silent.h>
#include <string>

class AccountBackend;

namespace account {

// Shared built-in lifecycle coordinator, not a required external plugin base.
class AccountModule : public IModule {
    Q_OBJECT

public:
    using Operation = rust::String (*)(rust::String);
    AccountModule(const char *plugin_id, Operation check, Operation remove);
    ~AccountModule() override = default;
    // Lists this module's configs off the GUI thread; answers with instances().
    auto list() -> ReqId override;
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
    void onListWorkerFinished();

protected:
    auto nextReqId() -> ReqId;
    // Launcher entries for the listed account ids, in listing order.
    [[nodiscard]] virtual auto instanceEntries(const QStringList &account_ids) const
        -> QList<QPair<QString, QString>>;
    void startListing();
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

    std::string m_plugin_id;
    ReqId m_next_req_id = 0;
    // One read at a time: requests made during a read wait for the next one, so every
    // reply comes from a read started after its request and replies keep request order.
    QPointer<QThread> m_list_worker;
    std::shared_ptr<QStringList> m_list_result;
    QList<ReqId> m_listing_req_ids;
    QList<ReqId> m_waiting_req_ids;
};

} // namespace account
