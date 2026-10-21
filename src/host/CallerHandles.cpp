#include "CallerHandles.h"

#include "Host.h"
#include "account/Spending.h"
#include "account/TransactionJob.h"
#include "i18n/Tr.h"

#include <Qontrol>
#include <common.h>

namespace host {

KeysHandle::KeysHandle(HostImpl *host, IModule *module, IKeys *target)
    : m_host(host),
      m_module(module),
      m_target(target) {
    connect(target, &QObject::destroyed, this, &KeysHandle::onTargetDestroyed, qontrol::UNIQUE);
    connect(target, &IKeys::coordinated, this, &KeysHandle::onCoordinated, qontrol::UNIQUE);
    connect(target, &IKeys::coordinationStarted, this, &KeysHandle::onCoordinationStarted,
            qontrol::UNIQUE);
    connect(target, &IKeys::attemptRetirement, this, &KeysHandle::onAttemptRetirement,
            qontrol::UNIQUE);
    connect(target, &IKeys::coordinationError, this, &KeysHandle::onCoordinationError,
            qontrol::UNIQUE);
    connect(target, &IKeys::signed_, this, &KeysHandle::onSigned, qontrol::UNIQUE);
    connect(target, &IKeys::verified, this, &KeysHandle::onVerified, qontrol::UNIQUE);
    connect(target, &IKeys::spDescriptorReady, this, &KeysHandle::onSpDescriptorReady,
            qontrol::UNIQUE);
    connect(target, &IKeys::xpubReady, this, &KeysHandle::onXpubReady, qontrol::UNIQUE);
    connect(target, &IKeys::descriptorRegistered, this, &KeysHandle::onDescriptorRegistered,
            qontrol::UNIQUE);
    connect(target, &IKeys::registrationChecked, this, &KeysHandle::onRegistrationChecked,
            qontrol::UNIQUE);
    connect(target, &IKeys::error, this, &KeysHandle::onError, qontrol::UNIQUE);
    connect(target, &IKeys::cancelled, this, &KeysHandle::onCancelled, qontrol::UNIQUE);
}

KeysHandle::~KeysHandle() {
    retire();
}

void KeysHandle::retire() {
    auto requests = m_requests;
    for (auto req : requests) {
        cancelRequest(req);
    }
    if (!m_retirement_jobs.empty()) {
        return;
    }
    for (const auto &request : m_attempts) {
        auto *job = new TransactionJob(0, TransactionJob::Operation::Release, request);
        m_retirement_jobs.insert(job);
        connect(job, &QThread::finished, this, &KeysHandle::onRetirementFinished, qontrol::UNIQUE);
        connect(job, &QThread::finished, job, &QObject::deleteLater, qontrol::UNIQUE);
        job->start();
    }
    m_target = nullptr;
}

void KeysHandle::release() {
    m_releasing = true;
    retire();
    if (m_retirement_jobs.empty() && m_attempts.empty()) {
        deleteLater();
    }
}

void KeysHandle::onRetirementFinished() {
    auto *job = dynamic_cast<TransactionJob *>(sender());
    if (job == nullptr || !m_retirement_jobs.remove(job)) {
        return;
    }
    const auto &result = job->result();
    onAttemptRetirement(job->sessionId(),
                        result.is_ok ? plugin::RetirementStatus::Retired
                                     : plugin::RetirementStatus::Failed,
                        QString::fromStdString(std::string(result.error)));
    if (m_releasing && m_retirement_jobs.empty() && m_attempts.empty()) {
        deleteLater();
    }
}

void KeysHandle::onCoordinationStarted(ReqId req_id, const plugin::SigningResult &prepared) {
    if (!m_requests.contains(req_id) || !m_coordination_requests.contains(req_id)) {
        return;
    }
    if (!prepared.sessionId.isEmpty() && !m_attempts.contains(prepared.sessionId)) {
        auto request = m_coordination_requests.value(req_id);
        request.psbtV2Hex = prepared.psbtV2Hex;
        request.sessionId = prepared.sessionId;
        m_attempts.insert(prepared.sessionId, request);
    }
    emit coordinationStarted(req_id, prepared);
}

void KeysHandle::onAttemptRetirement(const QString &session_id, plugin::RetirementStatus status,
                                     const QString &error) {
    if (!m_attempts.contains(session_id)) {
        return;
    }
    if (status == plugin::RetirementStatus::Retired) {
        m_attempts.remove(session_id);
    }
    if (status == plugin::RetirementStatus::Failed) {
        qCritical() << "MuSig attempt retirement failed; retaining valid snapshot for retry:"
                    << session_id << error;
    }
    emit attemptRetirement(session_id, status, error);
    if (m_releasing && m_retirement_jobs.empty() && m_attempts.empty()) {
        deleteLater();
    }
}

void KeysHandle::cancelRequest(ReqId req_id) {
    m_submitted.remove(req_id);
    m_coordination_requests.remove(req_id);
    if (!m_requests.remove(req_id) || m_target.isNull()) {
        return;
    }
    m_target->cancelRequest(req_id);
    emit cancelled(req_id);
}

void KeysHandle::coordinate(ReqId req_id, const plugin::SigningRequest &request) {
    if (m_submitted.contains(req_id)) {
        return;
    }
    if (!mine(req_id)) {
        emit coordinationError(req_id, plugin::SpendError::InvalidRequest,
                               QStringLiteral("request was not allocated by this caller"));
        return;
    }
    if (m_target == nullptr || !m_host->allowed(m_module, Grant::Keys, true)) {
        m_requests.remove(req_id);
        emit coordinationError(req_id, plugin::SpendError::Permission,
                               QStringLiteral("keys unavailable or access denied"));
        return;
    }
    if (!account::validRequest(request)) {
        m_requests.remove(req_id);
        emit coordinationError(req_id, plugin::SpendError::InvalidRequest,
                               QStringLiteral("invalid public enum value"));
        return;
    }
    CallerScope scope(m_target.data(), HostImpl::caller(m_module));
    auto metadata = m_target->keys(request.network);
    if (!metadata.is_ok) {
        m_requests.remove(req_id);
        emit coordinationError(req_id, plugin::SpendError::InvalidRequest, metadata.error);
        return;
    }
    setCaller(HostImpl::caller(m_module));
    m_submitted.insert(req_id);
    m_coordination_requests.insert(req_id, request);
    m_target->coordinate(req_id, request);
}

void KeysHandle::onCoordinated(ReqId req_id, const plugin::SigningResult &result) {
    if (!m_requests.remove(req_id)) {
        return;
    }
    m_submitted.remove(req_id);
    auto request = m_coordination_requests.take(req_id);
    if (request.stage == plugin::SigningStage::Release) {
        m_attempts.remove(request.sessionId);
    } else if (!result.sessionId.isEmpty()) {
        if (result.progress == plugin::SigningProgress::Complete) {
            m_attempts.remove(result.sessionId);
        } else {
            request.psbtV2Hex = result.psbtV2Hex;
            request.sessionId = result.sessionId;
            m_attempts.insert(result.sessionId, request);
        }
    }
    if (m_host->allowed(m_module, Grant::Keys, false)) {
        emit coordinated(req_id, result);
    } else {
        retire();
        emit coordinationError(req_id, plugin::SpendError::Permission,
                               QStringLiteral("keys permission revoked"));
    }
}

void KeysHandle::onCoordinationError(ReqId req_id, plugin::SpendError kind,
                                     const QString &message) {
    m_submitted.remove(req_id);
    m_coordination_requests.remove(req_id);
    if (m_requests.remove(req_id)) {
        emit coordinationError(req_id, kind, message);
    }
}

TransactionsHandle::TransactionsHandle(HostImpl *host, IModule *module)
    : m_host(host),
      m_module(module) {
}

auto TransactionsHandle::newRequest() -> ReqId {
    if (m_retired) {
        return 0;
    }
    auto reqId = ++m_next_req_id;
    m_allocated.insert(reqId);
    return reqId;
}

void TransactionsHandle::cancelRequest(ReqId req_id) {
    auto allocated = m_allocated.remove(req_id);
    auto pending = m_pending.remove(req_id);
    if (allocated || pending) {
        emit cancelled(req_id);
    }
}

void TransactionsHandle::retire() {
    m_retired = true;
    auto requests = m_allocated | m_pending;
    for (auto reqId : requests) {
        cancelRequest(reqId);
    }
}

auto TransactionsHandle::begin(ReqId req_id) -> bool {
    if (m_pending.contains(req_id)) {
        return false;
    }
    if (m_retired || !m_allocated.remove(req_id)) {
        emit error(req_id, plugin::SpendError::InvalidRequest,
                   QStringLiteral("request is unknown or already submitted"));
        return false;
    }
    if (!m_host->allowed(m_module, Grant::Account, true)) {
        emit error(req_id, plugin::SpendError::Permission, QStringLiteral("account access denied"));
        return false;
    }
    m_pending.insert(req_id);
    return true;
}

void TransactionsHandle::finalize(ReqId req_id, const plugin::SigningRequest &request) {
    if (!begin(req_id)) {
        return;
    }
    auto *job = new TransactionJob(req_id, TransactionJob::Operation::Finalize, request);
    connect(job, &QThread::finished, this, &TransactionsHandle::onFinished, qontrol::UNIQUE);
    connect(job, &QThread::finished, job, &QObject::deleteLater, qontrol::UNIQUE);
    job->start();
}

void TransactionsHandle::broadcast(ReqId req_id, plugin::Network network, const QString &endpoint,
                                   const QString &tx_hex) {
    if (!begin(req_id)) {
        return;
    }
    plugin::SigningRequest request;
    request.network = network;
    auto *job =
        new TransactionJob(req_id, TransactionJob::Operation::Broadcast, request, endpoint, tx_hex);
    connect(job, &QThread::finished, this, &TransactionsHandle::onFinished, qontrol::UNIQUE);
    connect(job, &QThread::finished, job, &QObject::deleteLater, qontrol::UNIQUE);
    job->start();
}

void TransactionsHandle::onFinished() {
    auto *job = dynamic_cast<TransactionJob *>(sender());
    if (job == nullptr || !m_pending.remove(job->requestId())) {
        return;
    }
    auto reqId = job->requestId();
    if (!m_host->allowed(m_module, Grant::Account, false)) {
        emit error(reqId, plugin::SpendError::Permission,
                   QStringLiteral("account permission revoked"));
        return;
    }
    const auto &result = job->result();
    if (!result.is_ok) {
        emit error(reqId, account::toPublic<plugin::SpendError>(result.kind),
                   QString::fromStdString(std::string(result.error)));
    } else if (job->operation() == TransactionJob::Operation::Broadcast) {
        emit broadcasted(reqId, QString::fromStdString(std::string(result.txid)));
    } else {
        emit finalized(reqId, QString::fromStdString(std::string(result.psbt_v2_hex)),
                       QString::fromStdString(std::string(result.tx_hex)),
                       QString::fromStdString(std::string(result.txid)));
    }
}

auto KeysHandle::allowedTarget(std::optional<ReqId> req_id, plugin::Network network) const
    -> IKeys * {
    auto *self = const_cast<KeysHandle *>(this);
    if (req_id.has_value()) {
        if (m_submitted.contains(*req_id)) {
            return nullptr;
        }
        if (!mine(*req_id)) {
            emit self->error(req_id, QStringLiteral("request was not allocated by this caller"));
            return nullptr;
        }
        self->m_submitted.insert(*req_id);
    }
    if (!plugin::enumToWire(network).has_value()) {
        if (req_id.has_value()) {
            self->m_requests.remove(*req_id);
            self->m_submitted.remove(*req_id);
        }
        emit self->error(req_id, QStringLiteral("invalid network value"),
                         plugin::SignError::InvalidRequest);
        return nullptr;
    }
    if (m_target == nullptr) {
        if (req_id.has_value()) {
            emit self->error(req_id, TR("plugin-access-no-keys"));
        }
        return nullptr;
    }
    if (!m_host->allowed(m_module, Grant::Keys, true)) {
        if (req_id.has_value()) {
            emit self->error(req_id, TR("plugin-access-refused-keys"));
        }
        return nullptr;
    }
    if (req_id.has_value()) {
        CallerScope scope(m_target.data(), HostImpl::caller(m_module));
        auto metadata = m_target->keys(network);
        if (!metadata.is_ok) {
            self->m_requests.remove(*req_id);
            self->m_submitted.remove(*req_id);
            emit self->error(req_id, metadata.error, metadata.kind);
            return nullptr;
        }
    }
    return m_target;
}

auto KeysHandle::mine(ReqId req_id) const -> bool {
    return m_requests.contains(req_id);
}

auto KeysHandle::finish(ReqId req_id) -> bool {
    if (!m_requests.remove(req_id)) {
        return false;
    }
    m_submitted.remove(req_id);
    if (!m_host->allowed(m_module, Grant::Keys, false)) {
        emit error(req_id, QStringLiteral("keys permission revoked"));
        return false;
    }
    return true;
}

void KeysHandle::onTargetDestroyed() {
    auto requests = m_requests;
    for (auto reqId : requests) {
        if (m_coordination_requests.contains(reqId)) {
            emit coordinationError(reqId, plugin::SpendError::LostSession,
                                   QStringLiteral("keys service removed"));
        } else {
            emit error(reqId, QStringLiteral("keys service removed"),
                       plugin::SignError::MusigSession);
        }
    }
    m_requests.clear();
    m_coordination_requests.clear();
    m_submitted.clear();
    retire();
}

auto KeysHandle::readKeys(plugin::Network network) const -> KeyListResult {
    auto *target = allowedTarget(std::nullopt, network);
    if (target == nullptr) {
        return {.error = QStringLiteral("keys unavailable or access denied")};
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->keys(network);
}

auto KeysHandle::newRequest() -> ReqId {
    if (m_target == nullptr) {
        return 0;
    }
    ReqId reqId = m_target->newRequest();
    if (reqId != 0) {
        m_requests.insert(reqId);
    }
    return reqId;
}

void KeysHandle::sign(ReqId req_id, plugin::Network network, const QString &psbt_v2_hex,
                      const QStringList &descriptors, const QString &wallet_name,
                      const QString &fingerprint) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->sign(req_id, network, psbt_v2_hex, descriptors, wallet_name, fingerprint);
}

void KeysHandle::signOnDevice(ReqId req_id, plugin::Network network, const QString &psbt_v2_hex,
                              const QString &descriptor, const QString &fingerprint) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->signOnDevice(req_id, network, psbt_v2_hex, descriptor, fingerprint);
}

void KeysHandle::verifyAddress(ReqId req_id, plugin::Network network, const QString &descriptor,
                               const QString &path, const QString &address,
                               const QString &fingerprint) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->verifyAddress(req_id, network, descriptor, path, address, fingerprint);
}

void KeysHandle::verifyOnDevice(ReqId req_id, plugin::Network network, const QString &descriptor,
                                const QString &path, const QString &address,
                                const QString &fingerprint) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->verifyOnDevice(req_id, network, descriptor, path, address, fingerprint);
}

void KeysHandle::spDescriptor(ReqId req_id, plugin::Network network, const QString &fingerprint) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->spDescriptor(req_id, network, fingerprint);
}

void KeysHandle::xpub(ReqId req_id, plugin::Network network, const QString &fingerprint,
                      const QString &path) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->xpub(req_id, network, fingerprint, path);
}

void KeysHandle::xpubOnDevice(ReqId req_id, plugin::Network network, const QString &fingerprint,
                              const QString &path) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->xpubOnDevice(req_id, network, fingerprint, path);
}

auto KeysHandle::cachedXpub(plugin::Network network, const QString &fingerprint,
                            const QString &path) const -> QString {
    auto *target = allowedTarget(std::nullopt, network);
    if (target == nullptr) {
        return {};
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->cachedXpub(network, fingerprint, path);
}

auto KeysHandle::registrationHmac(plugin::Network network, const QString &fingerprint,
                                  const QString &descriptor) const -> QString {
    auto *target = allowedTarget(std::nullopt, network);
    if (target == nullptr) {
        return {};
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->registrationHmac(network, fingerprint, descriptor);
}

auto KeysHandle::storeRegistrationHmac(plugin::Network network, const QString &fingerprint,
                                       const QString &descriptor, const QString &hmac) -> bool {
    auto *target = allowedTarget(std::nullopt, network);
    if (target == nullptr) {
        return false;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->storeRegistrationHmac(network, fingerprint, descriptor, hmac);
}

void KeysHandle::registerDescriptor(ReqId req_id, plugin::Network network,
                                    const QString &fingerprint, const QString &descriptor) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->registerDescriptor(req_id, network, fingerprint, descriptor);
}

void KeysHandle::checkDescriptorRegistered(ReqId req_id, plugin::Network network,
                                           const QString &fingerprint, const QString &descriptor) {
    auto *target = allowedTarget(req_id, network);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->checkDescriptorRegistered(req_id, network, fingerprint, descriptor);
}

void KeysHandle::onSigned(ReqId req_id, const QString &psbt_v2_hex) {
    if (finish(req_id)) {
        emit signed_(req_id, psbt_v2_hex);
    }
}

void KeysHandle::onVerified(ReqId req_id, bool verified) {
    if (finish(req_id)) {
        emit IKeys::verified(req_id, verified);
    }
}

void KeysHandle::onSpDescriptorReady(ReqId req_id, const QString &descriptor,
                                     const QString &fingerprint) {
    if (finish(req_id)) {
        emit spDescriptorReady(req_id, descriptor, fingerprint);
    }
}

void KeysHandle::onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                             const QString &path) {
    if (finish(req_id)) {
        emit xpubReady(req_id, xpub, fingerprint, path);
    }
}

void KeysHandle::onDescriptorRegistered(ReqId req_id, const QString &fingerprint) {
    if (finish(req_id)) {
        emit descriptorRegistered(req_id, fingerprint);
    }
}

void KeysHandle::onRegistrationChecked(ReqId req_id, bool registered) {
    if (finish(req_id)) {
        emit registrationChecked(req_id, registered);
    }
}

void KeysHandle::onError(std::optional<ReqId> req_id, const QString &message,
                         plugin::SignError kind) {
    // A failure tied to no request goes to every caller allowed to use keys.
    bool forMe =
        req_id.has_value() ? finish(*req_id) : m_host->allowed(m_module, Grant::Keys, false);
    if (forMe) {
        emit error(req_id, message, kind);
    }
}

void KeysHandle::onCancelled(ReqId req_id) {
    if (finish(req_id)) {
        emit cancelled(req_id);
    }
}

AccountHandle::AccountHandle(HostImpl *host, IModule *module, IAccount *target)
    : m_host(host),
      m_module(module),
      m_target(target) {
    connect(target, &QObject::destroyed, this, &AccountHandle::onAccountDestroyed, qontrol::UNIQUE);
    connect(target, &IAccount::cancelled, this, &AccountHandle::onAccountCancelled,
            qontrol::UNIQUE);
    connect(target, qOverload<ReqId, QString>(&IAccount::name), this, &AccountHandle::onName,
            qontrol::UNIQUE);
    connect(target, qOverload<ReqId, QMap<QString, QString>>(&IAccount::info), this,
            &AccountHandle::onInfo, qontrol::UNIQUE);
    connect(target, qOverload<ReqId, QList<plugin::Coin>>(&IAccount::coins), this,
            &AccountHandle::onCoins, qontrol::UNIQUE);
    connect(target, qOverload<ReqId, plugin::Balance>(&IAccount::balance), this,
            &AccountHandle::onBalance, qontrol::UNIQUE);
    connect(target, &IAccount::receiveAddress, this, &AccountHandle::onReceiveAddress,
            qontrol::UNIQUE);
    connect(target, &IAccount::changeAddress, this, &AccountHandle::onChangeAddress,
            qontrol::UNIQUE);
    connect(target, qOverload<ReqId, QString>(&IAccount::descriptor), this,
            &AccountHandle::onDescriptor, qontrol::UNIQUE);
    connect(target, qOverload<ReqId, QByteArray>(&IAccount::raw), this, &AccountHandle::onRaw,
            qontrol::UNIQUE);
    connect(target, &IAccount::error, this, &AccountHandle::onError, qontrol::UNIQUE);
}

bool AccountHandle::implemented() const {
    return m_target != nullptr && m_target->implemented();
}

AccountHandle::~AccountHandle() {
    auto requests = m_requests;
    for (auto reqId : requests) {
        cancelRequest(reqId);
    }
}

void AccountHandle::cancelRequest(ReqId req_id) {
    if (!m_requests.remove(req_id)) {
        return;
    }
    if (m_target != nullptr) {
        CallerScope scope(m_target.data(), HostImpl::caller(m_module));
        m_target->cancelRequest(req_id);
    }
    emit cancelled(req_id);
}

void AccountHandle::onAccountDestroyed() {
    auto requests = std::exchange(m_requests, {});
    m_held.clear();
    for (auto reqId : requests) {
        emit cancelled(reqId);
    }
}

void AccountHandle::onAccountCancelled(ReqId req_id) {
    relay(req_id, [this, req_id]() { emit cancelled(req_id); });
}

auto AccountHandle::forward(const std::function<ReqId(IAccount *)> &request) -> ReqId {
    if (m_target == nullptr) {
        emit error(std::nullopt, TR("plugin-access-no-account"));
        return 0;
    }
    if (!m_host->allowed(m_module, Grant::Account, true)) {
        emit error(std::nullopt, TR("plugin-access-refused-account"));
        return 0;
    }
    ReqId reqId = 0;
    ++m_in_call;
    {
        CallerScope scope(m_target.data(), HostImpl::caller(m_module));
        reqId = request(m_target);
    }
    --m_in_call;
    if (reqId != 0) {
        m_requests.insert(reqId);
    }
    if (m_in_call == 0) {
        auto held = std::exchange(m_held, {});
        for (const auto &[held_id, emit_answer] : held) {
            relay(held_id, emit_answer);
        }
    }
    return reqId;
}

void AccountHandle::relay(std::optional<ReqId> req_id, std::function<void()> emit_answer) {
    bool pushed = !req_id.has_value() || *req_id == 0;
    if (pushed) {
        // An update the account pushes goes to every caller allowed to use accounts.
        if (m_host->allowed(m_module, Grant::Account, false)) {
            emit_answer();
        }
        return;
    }
    if (m_in_call > 0) {
        m_held.append({req_id, std::move(emit_answer)});
        return;
    }
    if (m_requests.remove(*req_id)) {
        if (m_host->allowed(m_module, Grant::Account, false)) {
            emit_answer();
        } else {
            emit error(req_id, TR("plugin-access-refused-account"));
        }
    }
}

ReqId AccountHandle::name() {
    return forward([](IAccount *account) { return account->name(); });
}

ReqId AccountHandle::info() {
    return forward([](IAccount *account) { return account->info(); });
}

ReqId AccountHandle::coins() {
    return forward([](IAccount *account) { return account->coins(); });
}

ReqId AccountHandle::balance() {
    return forward([](IAccount *account) { return account->balance(); });
}

ReqId AccountHandle::newReceiveAddress() {
    return forward([](IAccount *account) { return account->newReceiveAddress(); });
}

ReqId AccountHandle::newChangeAddress() {
    return forward([](IAccount *account) { return account->newChangeAddress(); });
}

ReqId AccountHandle::descriptor() {
    return forward([](IAccount *account) { return account->descriptor(); });
}

ReqId AccountHandle::raw(const QByteArray &request) {
    return forward([&request](IAccount *account) { return account->raw(request); });
}

void AccountHandle::onName(ReqId req_id, const QString &name) {
    relay(req_id, [this, req_id, name]() { emit IAccount::name(req_id, name); });
}

void AccountHandle::onInfo(ReqId req_id, const QMap<QString, QString> &info) {
    relay(req_id, [this, req_id, info]() { emit IAccount::info(req_id, info); });
}

void AccountHandle::onCoins(ReqId req_id, const QList<plugin::Coin> &coins) {
    relay(req_id, [this, req_id, coins]() { emit IAccount::coins(req_id, coins); });
}

void AccountHandle::onBalance(ReqId req_id, plugin::Balance balance) {
    relay(req_id, [this, req_id, balance]() { emit IAccount::balance(req_id, balance); });
}

void AccountHandle::onReceiveAddress(ReqId req_id, const QString &address) {
    relay(req_id, [this, req_id, address]() { emit receiveAddress(req_id, address); });
}

void AccountHandle::onChangeAddress(ReqId req_id, const QString &address) {
    relay(req_id, [this, req_id, address]() { emit changeAddress(req_id, address); });
}

void AccountHandle::onDescriptor(ReqId req_id, const QString &descriptor) {
    relay(req_id, [this, req_id, descriptor]() { emit IAccount::descriptor(req_id, descriptor); });
}

void AccountHandle::onRaw(ReqId req_id, const QByteArray &response) {
    relay(req_id, [this, req_id, response]() { emit IAccount::raw(req_id, response); });
}

void AccountHandle::onError(std::optional<ReqId> req_id, const QString &message) {
    relay(req_id, [this, req_id, message]() { emit error(req_id, message); });
}

InstanceHandle::InstanceHandle(HostImpl *host, IModule *module, IInstance *target)
    : m_host(host),
      m_module(module),
      m_target(target) {
}

QString InstanceHandle::id() const {
    return m_target->id();
}

void InstanceHandle::stop() {
    m_target->stop();
}

IAccount *InstanceHandle::account() {
    auto *account = m_target->account();
    if (account == nullptr) {
        return nullptr;
    }
    if (m_account == nullptr) {
        m_account = std::make_unique<AccountHandle>(m_host, m_module, account);
    }
    return m_account.get();
}

IFeed *InstanceHandle::feed() {
    return m_target->feed();
}

ISigner *InstanceHandle::signer() {
    return m_target->signer();
}

IThemeProvider *InstanceHandle::theme() {
    return m_target->theme();
}

} // namespace host
