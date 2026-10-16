#pragma once

#include "interfaces/instance.h"
#include "interfaces/module.h"

#include <QMap>
#include <QString>
#include <memory>
#include <vector>

// One instance per config whose plugin id is "musig".
class MusigModule final : public IModule {
    Q_OBJECT

public:
    MusigModule();
    ~MusigModule() override = default;
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
