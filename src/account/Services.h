#pragma once

#include "host/ServiceAssembly.h"
#include "interfaces/backend/types.h"
#include "interfaces/enums.h"

#include <QString>
#include <QtGlobal>
#include <optional>

class IModule;

namespace backend {
class IAccountBackend;
class IBackendSettings;
class IBlockchain;
class ILabels;
class ITransaction;
} // namespace backend

namespace account {

// The services the shared account views use, each possibly from another provider.
struct Services {
    backend::IAccountBackend *account = nullptr;
    backend::IBlockchain *blockchain = nullptr;
    backend::ITransaction *transaction = nullptr;
    backend::ILabels *labels = nullptr;
    backend::IBackendSettings *settings = nullptr;
};

auto servicesOf(const host::ServiceAssembly &assembly) -> Services;

// The services `request` names, assembled by the host. Null, with the reason, when it
// refuses the assembly.
auto assembleServices(const host::AssemblyRequest &request) -> host::AssemblyResult;
// Every service of the account `id` on `network`, all from `module`.
auto moduleServices(IModule *module, const QString &id, backend::Network network)
    -> host::AssemblyRequest;

// The tip `state` counts confirmations from: its own, else the highest a connection
// reported. 0 when none is known.
auto stateTip(const backend::BlockchainState &state) -> quint32;

// The name of `network` in the current language.
auto networkName(backend::Network network) -> QString;

// The public network of `network`, nullopt for one the public interfaces do not know.
auto publicNetwork(backend::Network network) -> std::optional<plugin::Network>;

} // namespace account
