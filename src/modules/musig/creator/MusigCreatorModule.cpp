#include "modules/musig/creator/MusigCreatorModule.h"

#include "AppController.h"
#include "i18n/Tr.h"
#include "modules/musig/creator/CreatePolicy.h"

#include <QList>
#include <QPair>
#include <QStringList>
#include <optional>

namespace {
const QString K_CREATE_INSTANCE_ID = QStringLiteral("musig.create");
} // namespace

MusigCreatorModule::MusigCreatorModule() : IModule(nullptr) {
}

auto MusigCreatorModule::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), QStringLiteral("musig.creator")},
            {QStringLiteral("name"), QStringLiteral("MuSig2 Account Creator")},
            {QStringLiteral("icon"), QStringLiteral("users")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

auto MusigCreatorModule::list() -> ReqId {
    ReqId reqId = nextReqId();
    QList<QPair<QString, QString>> instances{{K_CREATE_INSTANCE_ID, TR("musig-account-type")}};
    emit IModule::instances(reqId, instances);
    return reqId;
}

auto MusigCreatorModule::autoStart() -> ReqId {
    ReqId reqId = nextReqId();
    emit IModule::autoStart(reqId, QStringList{});
    return reqId;
}

void MusigCreatorModule::startInstance(const QString &id) {
    if (id == K_CREATE_INSTANCE_ID) {
        AppController::execModal(new musig::modal::CreatePolicy);
        return;
    }
    emit error(std::nullopt, TR("musig-creator-unknown-instance").arg(id));
}

auto MusigCreatorModule::nextReqId() -> ReqId {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}
