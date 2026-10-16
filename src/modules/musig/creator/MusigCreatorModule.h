#pragma once

#include "interfaces/module.h"

#include <QMap>
#include <QString>

// Lists the pseudo instance "musig.create".
class MusigCreatorModule final : public IModule {
    Q_OBJECT

public:
    MusigCreatorModule();
    ~MusigCreatorModule() override = default;
    // id "musig.creator"
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override;
    auto list() -> ReqId override;
    auto autoStart() -> ReqId override;
    // "musig.create" opens modal::CreatePolicy, any other id is an error
    void startInstance(const QString &id) override;

protected:
    auto nextReqId() -> ReqId;

private:
    ReqId m_next_req_id = 0;
};
