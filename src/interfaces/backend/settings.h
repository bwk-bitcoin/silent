#pragma once

#include "service.h"
#include "types.h"

namespace backend {

class IBackendSettings : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~IBackendSettings() override = default;

    [[nodiscard]] virtual auto settings() const -> Settings = 0;

public slots:
    virtual void testEndpoint(backend::RequestId request_id, backend::Endpoint endpoint) = 0;

    virtual void save(backend::RequestId request_id, backend::Settings settings) = 0;

signals:
    void settingsChanged();
    void endpointTested(backend::RequestId request_id);
    void saved(backend::RequestId request_id);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IBackendSettings, "dev.silent.backend.IBackendSettings/1.0")
