#pragma once

#include "interfaces/instance.h"
#include "interfaces/module.h"

#include <memory>
#include <vector>

// Plugin id of a silent payments account's config.
inline constexpr auto SP_PLUGIN_ID = "sp";

class SpModule final : public IModule {
    Q_OBJECT

public:
    SpModule();
    ~SpModule() override;
    // The one SpModule, so its views name it as the caller of Host::keys() and
    // Host::instances(). Null before it is built.
    [[nodiscard]] static auto get() -> SpModule *;

    [[nodiscard]] QMap<QString, QString> meta() const override;
    ReqId list() override;
    ReqId autoStart() override;
    void startInstance(const QString &id) override;
    void deleteInstance(const QString &id) override;

private slots:
    void onInstanceRemoved(const QString &id);

protected:
    [[nodiscard]] static bool isSpAccount(const QString &account_id);

private:
    ReqId nextReqId();

    std::vector<std::unique_ptr<IInstance>> m_instances;
    ReqId m_next_req_id = 0;
};
