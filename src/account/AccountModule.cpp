#include "account/AccountModule.h"

#include "account/services/ServiceAdapters.h"
#include "account/services/ServiceContext.h"
#include "interfaces/host.h"
#include "modules/shared/AccountLifecycle.h"

#include <QPointer>
#include <QThread>
#include <Qontrol>
#include <common.h>
#include <utility>

namespace account {

AccountModule::AccountModule(const char *plugin_id, Operation check, Operation remove)
    : m_check(check),
      m_remove(remove),
      m_plugin_id(plugin_id) {
    connect(this, &AccountModule::deletionChecked, this, &AccountModule::onDeletionChecked,
            qontrol::UNIQUE);
    connect(this, &AccountModule::deletionRemoved, this, &AccountModule::onDeletionRemoved,
            qontrol::UNIQUE);
}

auto AccountModule::list() -> ReqId {
    ReqId reqId = nextReqId();
    m_waiting_req_ids.append(reqId);
    if (m_list_worker == nullptr) {
        startListing();
    }
    return reqId;
}

void AccountModule::startListing() {
    m_listing_req_ids = std::exchange(m_waiting_req_ids, {});
    auto result = std::make_shared<QStringList>();
    m_list_result = result;
    m_list_worker = QThread::create([result, pluginId = m_plugin_id] {
        for (auto &name : ::list_module_configs(pluginId)) {
            result->append(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        }
    });
    connect(m_list_worker, &QThread::finished, this, &AccountModule::onListWorkerFinished,
            qontrol::UNIQUE);
    connect(m_list_worker, &QThread::finished, m_list_worker, &QThread::deleteLater,
            qontrol::UNIQUE);
    m_list_worker->start();
}

void AccountModule::onListWorkerFinished() {
    if (sender() != m_list_worker.data()) {
        return;
    }
    m_list_worker = nullptr;
    auto result = std::move(m_list_result);
    auto entries = instanceEntries(*result);
    QPointer<AccountModule> alive(this);
    for (auto reqId : std::exchange(m_listing_req_ids, {})) {
        emit instances(reqId, entries);
        if (alive == nullptr) {
            return;
        }
    }
    if (m_list_worker == nullptr && !m_waiting_req_ids.isEmpty()) {
        startListing();
    }
}

auto AccountModule::instanceEntries(const QStringList &account_ids) const
    -> QList<QPair<QString, QString>> {
    QList<QPair<QString, QString>> entries;
    for (const auto &accountId : account_ids) {
        entries.append({accountId, accountId});
    }
    return entries;
}

auto AccountModule::nextReqId() -> ReqId {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}

auto AccountModule::accountUnavailable(const QString &id) const -> bool {
    return m_pending.contains(id) || (m_runtimes.contains(id) && !m_runtimes.value(id).stopped);
}

void AccountModule::watchAccount(const QString &id, AccountLifecycle *lifecycle,
                                 ServiceContext *services) {
    if (lifecycle == nullptr) {
        return;
    }
    m_runtimes.insert(id, {.lifecycle = lifecycle, .services = services});
    lifecycle->setProperty("logicalAccountId", id);
    connect(lifecycle, &AccountLifecycle::shutdownFinished, this,
            &AccountModule::onShutdownFinished, qontrol::UNIQUE);
}

auto AccountModule::service(const QString &account, backend::Network network,
                            backend::ServiceKind kind,
                            [[maybe_unused]] const std::optional<backend::AccountWatch> &watch)
    -> backend::IService * {
    auto runtime = m_runtimes.value(account);
    if (runtime.lifecycle == nullptr || runtime.stopped || m_pending.contains(account) ||
        runtime.lifecycle->accountLoading() || !runtime.lifecycle->hasAccountMetadata()) {
        return nullptr;
    }
    if (runtime.services == nullptr || runtime.services->network() != network) {
        return nullptr;
    }
    return serviceAdapter(kind, runtime.services, this);
}

void AccountModule::releaseService(backend::IService *service) {
    service->deleteLater();
}

void AccountModule::deleteAccount(ReqId req_id, const QString &id) {
    if (req_id == 0 || id.isEmpty() || m_pending.contains(id)) {
        emit accountDeleted(req_id, id, QStringLiteral("Invalid or pending account deletion"));
        return;
    }
    m_pending.insert(id, {.req_id = req_id});
    if (auto lifecycle = m_runtimes.value(id).lifecycle; lifecycle != nullptr) {
        lifecycle->setAccountDeletionPending(true);
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
    auto *lifecycle = qobject_cast<AccountLifecycle *>(sender());
    if (lifecycle == nullptr) {
        return;
    }
    auto id = lifecycle->property("logicalAccountId").toString();
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
    if (auto lifecycle = m_runtimes.value(id).lifecycle; lifecycle != nullptr) {
        lifecycle->setAccountDeletionPending(false);
    }
    auto pending = m_pending.take(id);
    emit accountDeleted(pending.req_id, id, error);
}

} // namespace account
