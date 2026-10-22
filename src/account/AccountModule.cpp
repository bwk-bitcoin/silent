#include "account/AccountModule.h"

#include "account/AccountBackend.h"
#include "interfaces/host.h"

#include <QThread>
#include <Qontrol>
#include <common.h>

namespace account {

AccountModule::AccountModule(Operation check, Operation remove) : m_check(check), m_remove(remove) {
    connect(this, &AccountModule::deletionChecked, this, &AccountModule::onDeletionChecked,
            qontrol::UNIQUE);
    connect(this, &AccountModule::deletionRemoved, this, &AccountModule::onDeletionRemoved,
            qontrol::UNIQUE);
}

auto AccountModule::accountUnavailable(const QString &id) const -> bool {
    return m_pending.contains(id) || (m_runtimes.contains(id) && !m_runtimes.value(id).stopped);
}

void AccountModule::watchAccount(const QString &id, AccountBackend *backend) {
    if (backend == nullptr) {
        return;
    }
    m_runtimes.insert(id, {.backend = backend});
    backend->setProperty("logicalAccountId", id);
    connect(backend, &AccountBackend::shutdownFinished, this, &AccountModule::onShutdownFinished,
            qontrol::UNIQUE);
}

void AccountModule::deleteAccount(ReqId req_id, const QString &id) {
    if (req_id == 0 || id.isEmpty() || m_pending.contains(id)) {
        emit accountDeleted(req_id, id, QStringLiteral("Invalid or pending account deletion"));
        return;
    }
    m_pending.insert(id, {.req_id = req_id});
    if (auto backend = m_runtimes.value(id).backend; backend != nullptr) {
        backend->setAccountDeletionPending(true);
    }
    auto *worker = QThread::create([this, id, check = m_check] {
        auto error = check(rust::String(id.toStdString()));
        emit deletionChecked(id, QString::fromUtf8(error.data(), error.size()));
    });
    connect(worker, &QThread::finished, worker, &QThread::deleteLater, qontrol::UNIQUE);
    worker->start();
}

void AccountModule::onDeletionChecked(const QString &id, const QString &error) {
    if (!m_pending.contains(id)) {
        return;
    }
    if (!error.isEmpty()) {
        finishDeletion(id, error);
        return;
    }
    m_pending[id].validated = true;
    auto *host = Host::get();
    if (host != nullptr) {
        host->removeInstance(id, this);
    }
    auto runtime = m_runtimes.value(id);
    if (m_runtimes.contains(id) && !runtime.stopped) {
        return;
    }
    if (!runtime.error.isEmpty()) {
        finishDeletion(id, runtime.error);
        return;
    }
    removeAccountStorage(id);
}

void AccountModule::onShutdownFinished(const QString &error) {
    auto *backend = qobject_cast<AccountBackend *>(sender());
    if (backend == nullptr) {
        return;
    }
    auto id = backend->property("logicalAccountId").toString();
    auto &runtime = m_runtimes[id];
    runtime.stopped = true;
    runtime.error = error;
    if (m_pending.contains(id) && m_pending.value(id).validated) {
        if (error.isEmpty()) {
            removeAccountStorage(id);
        } else {
            finishDeletion(id, error);
        }
    } else if (!m_pending.contains(id) && error.isEmpty()) {
        m_runtimes.remove(id);
    }
}

void AccountModule::removeAccountStorage(const QString &id) {
    if (!m_pending.contains(id) || m_pending.value(id).removing) {
        return;
    }
    m_pending[id].removing = true;
    auto *worker = QThread::create([this, id, remove = m_remove] {
        auto error = remove(rust::String(id.toStdString()));
        emit deletionRemoved(id, QString::fromUtf8(error.data(), error.size()));
    });
    connect(worker, &QThread::finished, worker, &QThread::deleteLater, qontrol::UNIQUE);
    worker->start();
}

void AccountModule::onDeletionRemoved(const QString &id, const QString &error) {
    if (!m_pending.contains(id) || !m_pending.value(id).removing) {
        return;
    }
    if (error.isEmpty()) {
        m_runtimes.remove(id);
    }
    finishDeletion(id, error);
}

void AccountModule::finishDeletion(const QString &id, const QString &error) {
    if (!m_pending.contains(id)) {
        return;
    }
    if (auto backend = m_runtimes.value(id).backend; backend != nullptr) {
        backend->setAccountDeletionPending(false);
    }
    auto pending = m_pending.take(id);
    emit accountDeleted(pending.req_id, id, error);
}

} // namespace account
