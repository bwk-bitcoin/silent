#include "account/Services.h"

#include "host/Host.h"
#include "interfaces/host.h"

#include <algorithm>

namespace account {

auto servicesOf(const host::ServiceAssembly &assembly) -> Services {
    return {.account = assembly.accountBackend(),
            .blockchain = assembly.blockchain(),
            .transaction = assembly.transaction(),
            .labels = assembly.labels(),
            .settings = assembly.settings()};
}

auto assembleServices(IModule *module, const QString &id, backend::Network network)
    -> host::AssemblyResult {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    if (host == nullptr) {
        return {.error = QStringLiteral("No host assembles account services")};
    }
    return host->assembleServices({.account = id,
                                   .network = network,
                                   .account_backend = module,
                                   .blockchain = module,
                                   .transaction = module,
                                   .labels = module,
                                   .settings = module});
}

auto stateTip(const backend::BlockchainState &state) -> quint32 {
    if (state.tip.has_value()) {
        return static_cast<quint32>(*state.tip);
    }
    quint64 tip = 0;
    for (const auto &connection : state.connections) {
        tip = std::max(tip, connection.tip.value_or(0));
    }
    return static_cast<quint32>(tip);
}

auto publicNetwork(backend::Network network) -> std::optional<plugin::Network> {
    switch (network) {
    case backend::Network::Bitcoin:
        return plugin::Network::Bitcoin;
    case backend::Network::Testnet:
        return plugin::Network::Testnet;
    case backend::Network::Signet:
        return plugin::Network::Signet;
    case backend::Network::Regtest:
        return plugin::Network::Regtest;
    case backend::Network::Testnet4:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace account
