#pragma once

#include "interfaces/account.h"
#include "interfaces/instance.h"
#include "interfaces/keys.h"

#include <QList>
#include <QPointer>
#include <QSet>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

class HostImpl;
class IModule;

namespace host {

enum class Grant : std::uint8_t { Account, Keys, State };

// Stamps the caller on `target` for one call, and puts back the one it replaced when
// the call returns: a request that runs a modal flow can see another one nested in it.
template <typename Target> class CallerScope {
public:
    CallerScope(Target *target, const plugin::Caller &caller)
        : m_target(target),
          m_previous(target->caller()) {
        target->setCaller(caller);
    }

    ~CallerScope() {
        if (m_target != nullptr) {
            m_target->setCaller(m_previous);
        }
    }

    CallerScope(const CallerScope &) = delete;
    auto operator=(const CallerScope &) -> CallerScope & = delete;
    CallerScope(CallerScope &&) = delete;
    auto operator=(CallerScope &&) -> CallerScope & = delete;

private:
    QPointer<Target> m_target;
    plugin::Caller m_previous;
};

// The IKeys a module gets from Host::keys(): forwards each request to the keys module
// with the caller stamped, refuses it with error() when the caller may not use keys,
// and passes on only the answers to the requests it allocated.
class KeysHandle final : public IKeys {
    Q_OBJECT

public:
    KeysHandle(HostImpl *host, IModule *module, IKeys *target);

    [[nodiscard]] auto target() const -> IKeys * {
        return m_target;
    }

    [[nodiscard]] QList<KeyEntry> keys(Network network) const override;
    auto newRequest() -> ReqId override;
    void sign(ReqId req_id, Network network, const QString &psbt_v2_hex,
              const QStringList &descriptors, const QString &wallet_name,
              const QString &fingerprint) override;
    void signOnDevice(ReqId req_id, Network network, const QString &psbt_v2_hex,
                      const QString &descriptor, const QString &fingerprint) override;
    void verifyAddress(ReqId req_id, Network network, const QString &descriptor,
                       const QString &path, const QString &address,
                       const QString &fingerprint) override;
    void verifyOnDevice(ReqId req_id, Network network, const QString &descriptor,
                        const QString &path, const QString &address,
                        const QString &fingerprint) override;
    void spDescriptor(ReqId req_id, Network network, const QString &fingerprint) override;
    void xpub(ReqId req_id, Network network, const QString &fingerprint,
              const QString &path) override;
    void xpubOnDevice(ReqId req_id, Network network, const QString &fingerprint,
                      const QString &path) override;
    [[nodiscard]] auto cachedXpub(Network network, const QString &fingerprint,
                                  const QString &path) const -> QString override;
    [[nodiscard]] auto registrationHmac(Network network, const QString &fingerprint,
                                        const QString &descriptor) const -> QString override;
    auto storeRegistrationHmac(Network network, const QString &fingerprint,
                               const QString &descriptor, const QString &hmac) -> bool override;
    void registerDescriptor(ReqId req_id, Network network, const QString &fingerprint,
                            const QString &descriptor) override;
    void checkDescriptorRegistered(ReqId req_id, Network network, const QString &fingerprint,
                                   const QString &descriptor) override;

private slots:
    void onSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onVerified(ReqId req_id, bool verified);
    void onSpDescriptorReady(ReqId req_id, const QString &descriptor, const QString &fingerprint);
    void onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                     const QString &path);
    void onDescriptorRegistered(ReqId req_id, const QString &fingerprint);
    void onRegistrationChecked(ReqId req_id, bool registered);
    void onError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onCancelled(ReqId req_id);

private:
    // The keys module, with the caller stamped, or null after answering error() when
    // the caller may not use keys or the keys module is gone.
    [[nodiscard]] auto allowedTarget(std::optional<ReqId> req_id) const -> IKeys *;
    [[nodiscard]] auto mine(ReqId req_id) const -> bool;

    HostImpl *m_host = nullptr;
    IModule *m_module = nullptr;
    QPointer<IKeys> m_target;
    QSet<ReqId> m_requests;
};

// The IAccount behind an InstanceHandle: forwards each request with the caller
// stamped, refuses it when the caller may not use accounts, and passes on the answers
// to the caller's own requests and the updates the account pushes (req_id 0).
class AccountHandle final : public IAccount {
    Q_OBJECT

public:
    AccountHandle(HostImpl *host, IModule *module, IAccount *target);

    [[nodiscard]] bool implemented() const override;
    ReqId name() override;
    ReqId info() override;
    ReqId coins() override;
    ReqId balance() override;
    ReqId newReceiveAddress() override;
    ReqId newChangeAddress() override;
    ReqId descriptor() override;
    ReqId raw(const QByteArray &request) override;

private slots:
    void onName(ReqId req_id, const QString &name);
    void onInfo(ReqId req_id, const QMap<QString, QString> &info);
    void onCoins(ReqId req_id, const QList<plugin::Coin> &coins);
    void onBalance(ReqId req_id, plugin::Balance balance);
    void onReceiveAddress(ReqId req_id, const QString &address);
    void onChangeAddress(ReqId req_id, const QString &address);
    void onDescriptor(ReqId req_id, const QString &descriptor);
    void onRaw(ReqId req_id, const QByteArray &response);
    void onError(std::optional<ReqId> req_id, const QString &message);

private:
    // Runs one request on the account: refused (0, error(std::nullopt, ...)) when the
    // caller may not use accounts. An account answers from inside the call before its
    // id is known, so the answers given during the call wait for it.
    auto forward(const std::function<ReqId(IAccount *)> &request) -> ReqId;
    // Passes an answer on now, or once the call underway has returned its id.
    void relay(std::optional<ReqId> req_id, std::function<void()> emit_answer);

    HostImpl *m_host = nullptr;
    IModule *m_module = nullptr;
    QPointer<IAccount> m_target;
    QSet<ReqId> m_requests;
    int m_in_call = 0;
    QList<std::pair<std::optional<ReqId>, std::function<void()>>> m_held;
};

// One live instance as a module sees it through Host::instances(): its account() is
// an AccountHandle, the other capabilities are the instance's own.
class InstanceHandle final : public IInstance {
public:
    InstanceHandle(HostImpl *host, IModule *module, IInstance *target);

    [[nodiscard]] auto target() const -> IInstance * {
        return m_target;
    }

    [[nodiscard]] QString id() const override;
    void stop() override;
    IAccount *account() override;
    IFeed *feed() override;
    ISigner *signer() override;
    IThemeProvider *theme() override;

private:
    HostImpl *m_host = nullptr;
    IModule *m_module = nullptr;
    IInstance *m_target = nullptr;
    std::unique_ptr<AccountHandle> m_account;
};

} // namespace host
