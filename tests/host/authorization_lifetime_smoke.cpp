#include "authorization_lifetime_smoke.h"

#include "SpSigningProbe.h"
#include "access_async_smoke.h"
#include "host/PluginAccess.h"
#include "interfaces/account.h"
#include "interfaces/instance.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHash>
#include <QPointer>
#include <QThread>
#include <common.h>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <utility>

#define private public
#include "account/TransactionJob.h"
#include "host/CallerHandles.h"
#include "host/Host.h"
#undef private

namespace smoke {
namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("authorization lifetime: %s", message);
    }
}

class CountingKeys final : public DeferredKeys {
public:
    void coordinate([[maybe_unused]] ReqId req_id,
                    [[maybe_unused]] const plugin::SigningRequest &request) override {
        ++coordinates;
    }

    int coordinates = 0;
};

class ClosingAccount final : public IAccount {
public:
    [[nodiscard]] auto implemented() const -> bool override {
        return true;
    }

    auto name() -> ReqId override {
        ++m_name_requests;
        if (m_driver != nullptr) {
            m_lock->hold();
            emit IAccount::name(1, QStringLiteral("held-first-reply"));
            emit IAccount::name(1, QStringLiteral("held-second-reply"));
            m_driver->start();
        }
        return 1;
    }

    void queueOnName(AppStateLock *lock, AuthorizationLifetimeDriver *driver) {
        m_lock = lock;
        m_driver = driver;
    }

    auto info() -> ReqId override {
        return 0;
    }

    auto coins() -> ReqId override {
        return 0;
    }

    auto balance() -> ReqId override {
        return 0;
    }

    auto newReceiveAddress() -> ReqId override {
        return 0;
    }

    auto newChangeAddress() -> ReqId override {
        return 0;
    }

    auto descriptor() -> ReqId override {
        return 0;
    }

    auto raw([[maybe_unused]] const QByteArray &request) -> ReqId override {
        return 0;
    }

    [[nodiscard]] auto nameRequests() const -> int {
        return m_name_requests;
    }

private:
    int m_name_requests = 0;
    AppStateLock *m_lock = nullptr;
    AuthorizationLifetimeDriver *m_driver = nullptr;
};

class ClosingInstance final : public IInstance {
public:
    explicit ClosingInstance(IAccount *account) : m_account(account) {
    }

    [[nodiscard]] auto id() const -> QString override {
        return QStringLiteral("authorization.close");
    }

    void stop() override {
    }

    auto account() -> IAccount * override {
        return m_account;
    }

    auto feed() -> IFeed * override {
        return nullptr;
    }

    auto signer() -> ISigner * override {
        return nullptr;
    }

    auto theme() -> IThemeProvider * override {
        return nullptr;
    }

private:
    IAccount *m_account;
};

void allowCapabilities(IModule *module) {
    auto known = HostImpl::caller(module);
    auto request = pluginaccess::request(known.pluginId, known.pluginHash);
    require(request.is_ok, "the external fixture reads its current epoch");
    request.access.account = PluginGrant::Allowed;
    request.access.keys = PluginGrant::Allowed;
    require(pluginaccess::save(request.access, request.storage_epoch).isEmpty(),
            "the external fixture has persisted Account and Keys grants");
}

void cancelKeysDuringAuthorization(HostImpl *host, IModule *module, bool coordinate) {
    CountingKeys target;
    host::KeysHandle handle(host, module, &target);
    auto request = handle.newRequest();
    AppStateLock lock;
    AuthorizationLifetimeDriver driver(
        host, module, &lock, AuthorizationLifetimeDriver::Action::CancelKeys, &handle, request);
    QObject::connect(&handle, &IKeys::cancelled, &driver, &AuthorizationLifetimeDriver::onCancelled,
                     qontrol::UNIQUE);
    lock.hold();
    driver.start();
    if (coordinate) {
        plugin::SigningRequest signing;
        signing.network = plugin::Network::Regtest;
        handle.coordinate(request, signing);
    } else {
        handle.sign(request, plugin::Network::Regtest, {}, {}, {}, {});
    }
    require(driver.acted() && driver.cancellations() == 1,
            "the accepted request was cancelled while permission I/O waited");
    require(target.coordinates == 0 && target.requests == 0,
            "coordinate and allowedTarget never submit the cancelled request");
}

enum class AccountDelivery { Forward, Pushed, Replay };

void closeAccountDuringAuthorization(HostImpl *host, IModule *module, AccountDelivery delivery) {
    ClosingAccount target;
    ClosingInstance instance(&target);
    host->registerInstance(instance.id(), &instance, module);
    IAccount *account = nullptr;
    for (auto *candidate : host->instances(module)) {
        if (candidate->id() == instance.id()) {
            account = candidate->account();
        }
    }
    require(account != nullptr, "the real host owns the test account handle");
    QPointer<IAccount> alive(account);
    AppStateLock lock;
    AuthorizationLifetimeDriver driver(host, module, &lock,
                                       AuthorizationLifetimeDriver::Action::CloseAccount, account,
                                       0, instance.id());
    QObject::connect(account, qOverload<ReqId, QString>(&IAccount::name), &driver,
                     &AuthorizationLifetimeDriver::onName, qontrol::UNIQUE);
    if (delivery == AccountDelivery::Replay) {
        target.queueOnName(&lock, &driver);
    } else {
        lock.hold();
        driver.start();
    }
    if (delivery == AccountDelivery::Pushed) {
        emit target.IAccount::name(0, QStringLiteral("must-not-be-delivered"));
    } else {
        require(account->name() == 0, "a closed account forward is refused after the wait");
    }
    require(driver.acted() && alive == nullptr && driver.replies() == 0,
            "closing the real instance deletes its handle and suppresses the resumed reply");
    require(target.nameRequests() == (delivery == AccountDelivery::Replay ? 1 : 0) &&
                host->instanceOwner(instance.id()) == nullptr,
            "no new account request or replay is dispatched after close");
    require(host->callerFramesIdle(module), "the deleted account handle leaves no native frame");
}

void deleteFinishedJobDuringAuthorization(HostImpl *host, IModule *module) {
    host::TransactionsHandle handle(host, module);
    auto request = handle.newRequest();
    handle.m_allocated.remove(request);
    handle.m_pending.insert(request);
    handle.m_running = 1;
    plugin::SigningRequest invalid;
    invalid.network = static_cast<plugin::Network>(255);
    auto *job = new TransactionJob(request, TransactionJob::Operation::Finalize, invalid);
    QPointer<TransactionJob> alive(job);
    AppStateLock lock;
    AuthorizationLifetimeDriver driver(
        host, module, &lock, AuthorizationLifetimeDriver::Action::DeleteTransaction, job, request);
    QObject::connect(job, &QThread::finished, &handle, &host::TransactionsHandle::onFinished,
                     qontrol::UNIQUE);
    QObject::connect(job, &QThread::finished, job, &QObject::deleteLater, qontrol::UNIQUE);
    QObject::connect(&handle, &ITransactions::error, &driver,
                     &AuthorizationLifetimeDriver::onTransactionError, qontrol::UNIQUE);
    lock.hold();
    driver.start();
    job->start();
    QElapsedTimer deadline;
    deadline.start();
    while (driver.replies() == 0 && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(driver.acted() && alive == nullptr && driver.replies() == 1,
            "the copied transaction result survives job deletion inside the permission wait");
}
} // namespace

AuthorizationLifetimeDriver::AuthorizationLifetimeDriver(HostImpl *host, IModule *module,
                                                         AppStateLock *lock, Action action,
                                                         QObject *target, ReqId request,
                                                         QString instance_id)
    : m_host(host),
      m_module(module),
      m_lock(lock),
      m_action(action),
      m_target(target),
      m_request(request),
      m_instance_id(std::move(instance_id)) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &AuthorizationLifetimeDriver::onAct, qontrol::UNIQUE);
}

void AuthorizationLifetimeDriver::start() {
    m_timer.start(5);
}

void AuthorizationLifetimeDriver::onAct() {
    if (m_host->m_grant_waits.isEmpty()) {
        m_timer.start(5);
        return;
    }
    require(!m_host->callerFramesIdle(m_module),
            "the caller frame is pinned during the nested wait");
    m_acted = true;
    switch (m_action) {
    case Action::CancelKeys:
        dynamic_cast<IKeys *>(m_target.data())->cancelRequest(m_request);
        break;
    case Action::CloseAccount:
        m_host->removeInstance(m_instance_id);
        break;
    case Action::DeleteTransaction:
        if (m_target != nullptr) {
            require(qobject_cast<QThread *>(m_target.data())->isFinished(),
                    "the worker finished before deletion");
            // Deliver finished's queued deleteLater, then its deferred deletion, inside the wait.
            QCoreApplication::sendPostedEvents(m_target.data(), QEvent::MetaCall);
            if (m_target != nullptr) {
                QCoreApplication::sendPostedEvents(m_target.data(), QEvent::DeferredDelete);
            }
        }
        require(m_target == nullptr,
                "the production finished/deleteLater chain deletes the job before resume");
        break;
    }
    m_lock->release();
}

void AuthorizationLifetimeDriver::onCancelled(ReqId request) {
    require(request == m_request, "cancellation belongs to the submitted request");
    ++m_cancellations;
}

void AuthorizationLifetimeDriver::onName([[maybe_unused]] ReqId request,
                                         [[maybe_unused]] const QString &name) {
    ++m_replies;
}

void AuthorizationLifetimeDriver::onTransactionError(ReqId request, plugin::SpendError kind,
                                                     const QString &message) {
    require(request == m_request && kind == plugin::SpendError::InvalidRequest &&
                message == QStringLiteral("invalid public enum value"),
            "the exact transaction failure survives job deletion inside authorization");
    ++m_replies;
}

void authorizationLifetimeSmoke(HostImpl *host, IModule *module) {
    allowCapabilities(module);
    cancelKeysDuringAuthorization(host, module, true);
    std::fprintf(stdout, "Cancelled coordinate during authorization: OK\n");
    cancelKeysDuringAuthorization(host, module, false);
    std::fprintf(stdout, "Cancelled sign during authorization: OK\n");
    closeAccountDuringAuthorization(host, module, AccountDelivery::Forward);
    std::fprintf(stdout, "Closed account during forwarding: OK\n");
    closeAccountDuringAuthorization(host, module, AccountDelivery::Pushed);
    std::fprintf(stdout, "Closed account during reply authorization: OK\n");
    closeAccountDuringAuthorization(host, module, AccountDelivery::Replay);
    std::fprintf(stdout, "Closed account during synchronous reply replay: OK\n");
    deleteFinishedJobDuringAuthorization(host, module);
    std::fprintf(stdout, "Deleted transaction job during reply authorization: OK\n");
}
} // namespace smoke
