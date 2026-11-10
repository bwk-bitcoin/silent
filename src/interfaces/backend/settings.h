#pragma once

#include "service.h"
#include "types.h"

namespace backend {

// The account's network and the servers its blockchain service syncs from.
class IBackendSettings : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~IBackendSettings() override = default;

    // The saved settings. Changes with settingsChanged().
    [[nodiscard]] virtual auto settings() const -> Settings = 0;

public slots:
    // Checks the server `endpoint` describes answers on the account's network, saving nothing
    // and leaving the live connection alone: answered by endpointTested(), or error() saying
    // why it failed.
    virtual void testEndpoint(backend::RequestId request_id, backend::Endpoint endpoint) = 0;

    // Saves `settings`, untested, answered by saved(). IBlockchain reconnects to the servers
    // saved. A built-in account refuses another network, and an endpoint it does not have.
    virtual void save(backend::RequestId request_id, backend::Settings settings) = 0;

signals:
    // settings() changed.
    void settingsChanged();
    // Answers testEndpoint(): the server answered.
    void endpointTested(backend::RequestId request_id);
    // Answers save(): the settings are saved.
    void saved(backend::RequestId request_id);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IBackendSettings, "dev.silent.backend.IBackendSettings/1.0")
