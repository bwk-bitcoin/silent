#pragma once

#include "interfaces/instance.h"
#include "interfaces/module.h"

#include <QMap>
#include <QString>
#include <memory>
#include <vector>

// Plugin id of a musig account's config (Rust MUSIG_PLUGIN_ID).
inline constexpr auto MUSIG_PLUGIN_ID = "musig";

// One instance per config whose plugin id is MUSIG_PLUGIN_ID.
class MusigModule final : public IModule {
    Q_OBJECT

public:
    MusigModule();
    ~MusigModule() override;
    // The one MusigModule, so its views name it as the caller of Host::keys() and
    // Host::instances(). Null before it is built.
    [[nodiscard]] static auto get() -> MusigModule *;
    // id "musig.module"
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override;
    auto list() -> ReqId override;
    auto autoStart() -> ReqId override;
    void startInstance(const QString &id) override;
    void deleteInstance(const QString &id) override;

public slots:
    void onInstanceRemoved(const QString &id);

protected:
    [[nodiscard]] static auto isMusigAccount(const QString &account_id) -> bool;
    auto nextReqId() -> ReqId;

private:
    std::vector<std::unique_ptr<IInstance>> m_instances;
    ReqId m_next_req_id = 0;
};
