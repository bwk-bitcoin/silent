#include "host/ServiceAssembly.h"

#include "host/Host.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "interfaces/module.h"

#include <QMetaType>
#include <algorithm>
#include <optional>
#include <utility>

namespace host {
namespace {

auto index(backend::ServiceKind kind) -> std::size_t {
    return static_cast<std::size_t>(kind);
}

} // namespace

ServiceAssembly::ServiceAssembly(HostImpl *host, QString account, backend::Network network)
    : m_host(host),
      m_account(std::move(account)),
      m_network(network) {
}

ServiceAssembly::~ServiceAssembly() {
    // Emptied first, so uses() is already false for anything a release triggers.
    auto released = std::exchange(m_supplied, {});
    for (auto &supplied : released) {
        auto *provider = qobject_cast<backend::IServiceProvider *>(supplied.provider.data());
        if (provider == nullptr || supplied.service == nullptr) {
            continue;
        }
        if (m_host != nullptr) {
            m_host->enterFrame(supplied.provider);
        }
        provider->releaseService(supplied.service);
        if (m_host != nullptr) {
            m_host->leaveFrame(supplied.provider);
        }
    }
}

auto ServiceAssembly::account() const -> QString {
    return m_account;
}

auto ServiceAssembly::network() const -> backend::Network {
    return m_network;
}

auto ServiceAssembly::accountBackend() const -> backend::IAccountBackend * {
    return qobject_cast<backend::IAccountBackend *>(service(backend::ServiceKind::Account));
}

auto ServiceAssembly::blockchain() const -> backend::IBlockchain * {
    return qobject_cast<backend::IBlockchain *>(service(backend::ServiceKind::Blockchain));
}

auto ServiceAssembly::transaction() const -> backend::ITransaction * {
    return qobject_cast<backend::ITransaction *>(service(backend::ServiceKind::Transaction));
}

auto ServiceAssembly::labels() const -> backend::ILabels * {
    return qobject_cast<backend::ILabels *>(service(backend::ServiceKind::Labels));
}

auto ServiceAssembly::settings() const -> backend::IBackendSettings * {
    return qobject_cast<backend::IBackendSettings *>(service(backend::ServiceKind::Settings));
}

auto ServiceAssembly::supplier(backend::ServiceKind kind) const -> plugin::Caller {
    return m_supplied.at(index(kind)).caller;
}

auto ServiceAssembly::uses(const IModule *module) const -> bool {
    return std::ranges::any_of(m_supplied, [module](const Supplied &supplied) {
        return supplied.provider != nullptr && supplied.provider.data() == module;
    });
}

void ServiceAssembly::supply(backend::ServiceKind kind, Supplied supplied) {
    m_supplied.at(index(kind)) = std::move(supplied);
}

auto ServiceAssembly::service(backend::ServiceKind kind) const -> backend::IService * {
    return m_supplied.at(index(kind)).service;
}

void registerServiceTypes() {
    qRegisterMetaType<backend::RequestId>("backend::RequestId");
    qRegisterMetaType<std::optional<quint64>>();
    qRegisterMetaType<backend::Network>();
    qRegisterMetaType<backend::TransactionTemplate>();
    qRegisterMetaType<backend::CraftResult>();
    qRegisterMetaType<backend::TransactionDetails>();
    qRegisterMetaType<backend::BroadcastResult>();
    qRegisterMetaType<backend::LabelKey>();
    qRegisterMetaType<backend::Address>();
    qRegisterMetaType<backend::Endpoint>();
    qRegisterMetaType<backend::Settings>();
    qRegisterMetaType<backend::BackupInfo>();
}

} // namespace host
