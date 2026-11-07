#pragma once

#include "interfaces/types.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class HostImpl;
class IModule;

namespace smoke {
class AppStateLock;

class AuthorizationLifetimeDriver final : public QObject {
    Q_OBJECT

public:
    enum class Action { CancelKeys, CloseAccount, DeleteTransaction };
    AuthorizationLifetimeDriver(HostImpl *host, IModule *module, AppStateLock *lock, Action action,
                                QObject *target, ReqId request, QString instance_id = {});
    void start();

    [[nodiscard]] auto acted() const -> bool {
        return m_acted;
    }

    [[nodiscard]] auto cancellations() const -> int {
        return m_cancellations;
    }

    [[nodiscard]] auto replies() const -> int {
        return m_replies;
    }

public slots:
    void onAct();
    void onCancelled(ReqId request);
    void onName(ReqId request, const QString &name);
    void onTransactionError(ReqId request, plugin::SpendError kind, const QString &message);

private:
    HostImpl *m_host;
    IModule *m_module;
    AppStateLock *m_lock;
    Action m_action;
    QPointer<QObject> m_target;
    ReqId m_request;
    QString m_instance_id;
    QTimer m_timer;
    bool m_acted = false;
    int m_cancellations = 0;
    int m_replies = 0;
};

// Records the noncesPrepared answers a caller handle forwards.
class NoncesPreparedSpy final : public QObject {
    Q_OBJECT

public:
    [[nodiscard]] auto count() const -> int {
        return m_count;
    }

    [[nodiscard]] auto request() const -> ReqId {
        return m_request;
    }

    [[nodiscard]] auto fingerprint() const -> QString {
        return m_fingerprint;
    }

public slots:
    void onNoncesPrepared(ReqId request, const QString &fingerprint);

private:
    int m_count = 0;
    ReqId m_request = 0;
    QString m_fingerprint;
};

void authorizationLifetimeSmoke(HostImpl *host, IModule *module);
} // namespace smoke
