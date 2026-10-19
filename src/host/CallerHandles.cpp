#include "CallerHandles.h"

#include "Host.h"
#include "i18n/Tr.h"

#include <Qontrol>
#include <common.h>

namespace host {

KeysHandle::KeysHandle(HostImpl *host, IModule *module, IKeys *target)
    : m_host(host),
      m_module(module),
      m_target(target) {
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

auto KeysHandle::allowedTarget(std::optional<ReqId> req_id) const -> IKeys * {
    auto *self = const_cast<KeysHandle *>(this);
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
    return m_target;
}

auto KeysHandle::mine(ReqId req_id) const -> bool {
    return m_requests.contains(req_id);
}

QList<KeyEntry> KeysHandle::keys(Network network) const {
    auto *target = allowedTarget(std::nullopt);
    if (target == nullptr) {
        return {};
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->keys(network);
}

auto KeysHandle::newRequest() -> ReqId {
    if (m_target == nullptr) {
        return 0;
    }
    ReqId reqId = m_target->newRequest();
    m_requests.insert(reqId);
    return reqId;
}

void KeysHandle::sign(ReqId req_id, Network network, const QString &psbt_v2_hex,
                      const QStringList &descriptors, const QString &wallet_name,
                      const QString &fingerprint) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->sign(req_id, network, psbt_v2_hex, descriptors, wallet_name, fingerprint);
}

void KeysHandle::signOnDevice(ReqId req_id, Network network, const QString &psbt_v2_hex,
                              const QString &descriptor, const QString &fingerprint) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->signOnDevice(req_id, network, psbt_v2_hex, descriptor, fingerprint);
}

void KeysHandle::verifyAddress(ReqId req_id, Network network, const QString &descriptor,
                               const QString &path, const QString &address,
                               const QString &fingerprint) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->verifyAddress(req_id, network, descriptor, path, address, fingerprint);
}

void KeysHandle::verifyOnDevice(ReqId req_id, Network network, const QString &descriptor,
                                const QString &path, const QString &address,
                                const QString &fingerprint) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->verifyOnDevice(req_id, network, descriptor, path, address, fingerprint);
}

void KeysHandle::spDescriptor(ReqId req_id, Network network, const QString &fingerprint) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->spDescriptor(req_id, network, fingerprint);
}

void KeysHandle::xpub(ReqId req_id, Network network, const QString &fingerprint,
                      const QString &path) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->xpub(req_id, network, fingerprint, path);
}

void KeysHandle::xpubOnDevice(ReqId req_id, Network network, const QString &fingerprint,
                              const QString &path) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->xpubOnDevice(req_id, network, fingerprint, path);
}

auto KeysHandle::cachedXpub(Network network, const QString &fingerprint, const QString &path) const
    -> QString {
    auto *target = allowedTarget(std::nullopt);
    if (target == nullptr) {
        return {};
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->cachedXpub(network, fingerprint, path);
}

auto KeysHandle::registrationHmac(Network network, const QString &fingerprint,
                                  const QString &descriptor) const -> QString {
    auto *target = allowedTarget(std::nullopt);
    if (target == nullptr) {
        return {};
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->registrationHmac(network, fingerprint, descriptor);
}

auto KeysHandle::storeRegistrationHmac(Network network, const QString &fingerprint,
                                       const QString &descriptor, const QString &hmac) -> bool {
    auto *target = allowedTarget(std::nullopt);
    if (target == nullptr) {
        return false;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    return target->storeRegistrationHmac(network, fingerprint, descriptor, hmac);
}

void KeysHandle::registerDescriptor(ReqId req_id, Network network, const QString &fingerprint,
                                    const QString &descriptor) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->registerDescriptor(req_id, network, fingerprint, descriptor);
}

void KeysHandle::checkDescriptorRegistered(ReqId req_id, Network network,
                                           const QString &fingerprint, const QString &descriptor) {
    auto *target = allowedTarget(req_id);
    if (target == nullptr) {
        return;
    }
    CallerScope scope(target, HostImpl::caller(m_module));
    target->checkDescriptorRegistered(req_id, network, fingerprint, descriptor);
}

void KeysHandle::onSigned(ReqId req_id, const QString &psbt_v2_hex) {
    if (mine(req_id)) {
        emit signed_(req_id, psbt_v2_hex);
    }
}

void KeysHandle::onVerified(ReqId req_id, bool verified) {
    if (mine(req_id)) {
        emit IKeys::verified(req_id, verified);
    }
}

void KeysHandle::onSpDescriptorReady(ReqId req_id, const QString &descriptor,
                                     const QString &fingerprint) {
    if (mine(req_id)) {
        emit spDescriptorReady(req_id, descriptor, fingerprint);
    }
}

void KeysHandle::onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                             const QString &path) {
    if (mine(req_id)) {
        emit xpubReady(req_id, xpub, fingerprint, path);
    }
}

void KeysHandle::onDescriptorRegistered(ReqId req_id, const QString &fingerprint) {
    if (mine(req_id)) {
        emit descriptorRegistered(req_id, fingerprint);
    }
}

void KeysHandle::onRegistrationChecked(ReqId req_id, bool registered) {
    if (mine(req_id)) {
        emit registrationChecked(req_id, registered);
    }
}

void KeysHandle::onError(std::optional<ReqId> req_id, const QString &message,
                         plugin::SignError kind) {
    // A failure tied to no request goes to every caller allowed to use keys.
    bool forMe = req_id.has_value() ? mine(*req_id) : m_host->allowed(m_module, Grant::Keys, false);
    if (forMe) {
        emit error(req_id, message, kind);
    }
}

void KeysHandle::onCancelled(ReqId req_id) {
    if (mine(req_id)) {
        emit cancelled(req_id);
    }
}

AccountHandle::AccountHandle(HostImpl *host, IModule *module, IAccount *target)
    : m_host(host),
      m_module(module),
      m_target(target) {
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
    m_requests.insert(reqId);
    if (m_in_call == 0) {
        auto held = std::exchange(m_held, {});
        for (const auto &[held_id, emit_answer] : held) {
            if (!held_id.has_value() || *held_id == 0 || m_requests.contains(*held_id)) {
                emit_answer();
            }
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
    if (m_requests.contains(*req_id)) {
        emit_answer();
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
