#pragma once

#include "interfaces/backend/provider.h"
#include "interfaces/backend/service.h"
#include "interfaces/backend/types.h"
#include "interfaces/types.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <array>
#include <cstddef>
#include <memory>
#include <variant>

class HostImpl;
class IModule;

namespace backend {
class IAccountBackend;
class IBackendSettings;
class IBlockchain;
class ILabels;
class ITransaction;
} // namespace backend

namespace host {

// One assembled service per backend::ServiceKind.
constexpr auto SERVICE_KINDS = static_cast<std::size_t>(backend::ServiceKind::Settings) + 1;

// The host's own implementation, for a service no provider supplies: getters answer empty
// data and every request answers error().
struct Unsupported {};

// Where one assembled service comes from: a provider module, or explicitly no provider.
// Left unset (std::monostate), the assembly is refused.
using ServiceSource = std::variant<std::monostate, Unsupported, IModule *>;

// The services one account UI uses, each from its own source. Every provider is asked for
// this account on this network.
struct AssemblyRequest {
    QString account;
    backend::Network network = backend::Network::Bitcoin;
    ServiceSource account_backend;
    ServiceSource blockchain;
    ServiceSource transaction;
    ServiceSource labels;
    ServiceSource settings;
};

class ServiceAssembly;

struct AssemblyResult {
    std::unique_ptr<ServiceAssembly> assembly;
    QString error;
};

// The services of one account UI, built by HostImpl::assembleServices(): every getter
// answers a live object. Each supplying plugin cannot stop until the assembly is deleted,
// which hands every service back to its provider.
class ServiceAssembly final : public QObject {
    Q_OBJECT

public:
    ~ServiceAssembly() override;
    ServiceAssembly(const ServiceAssembly &) = delete;
    auto operator=(const ServiceAssembly &) -> ServiceAssembly & = delete;
    ServiceAssembly(ServiceAssembly &&) = delete;
    auto operator=(ServiceAssembly &&) -> ServiceAssembly & = delete;

    [[nodiscard]] auto account() const -> QString;
    [[nodiscard]] auto network() const -> backend::Network;
    [[nodiscard]] auto accountBackend() const -> backend::IAccountBackend *;
    [[nodiscard]] auto blockchain() const -> backend::IBlockchain *;
    [[nodiscard]] auto transaction() const -> backend::ITransaction *;
    [[nodiscard]] auto labels() const -> backend::ILabels *;
    [[nodiscard]] auto settings() const -> backend::IBackendSettings *;
    // Who supplied `kind`, as the registry attributes it: empty for the host's own.
    [[nodiscard]] auto supplier(backend::ServiceKind kind) const -> plugin::Caller;
    [[nodiscard]] auto uses(const IModule *module) const -> bool;

signals:
    // A supplier's plugin started stopping: drop every use of these services, then delete
    // the assembly so that plugin can finish.
    void retired();

protected:
    friend class ::HostImpl;

    struct Supplied {
        QPointer<backend::IService> service;
        // Null for the host's own implementation, which the assembly owns.
        QPointer<IModule> provider;
        plugin::Caller caller;
    };

    ServiceAssembly(HostImpl *host, QString account, backend::Network network);
    void supply(backend::ServiceKind kind, Supplied supplied);
    [[nodiscard]] auto service(backend::ServiceKind kind) const -> backend::IService *;

private:
    QPointer<HostImpl> m_host;
    QString m_account;
    backend::Network m_network;
    std::array<Supplied, SERVICE_KINDS> m_supplied;
};

// Registers every service payload for queued delivery. Called once, before any module
// activates.
void registerServiceTypes();

} // namespace host
