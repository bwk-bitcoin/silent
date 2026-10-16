#include "modules/musig/creator/MusigCreatorModule.h"

MusigCreatorModule::MusigCreatorModule() : IModule(nullptr) {
}

auto MusigCreatorModule::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), QStringLiteral("musig.creator")},
            {QStringLiteral("name"), QStringLiteral("MuSig2 Account Creator")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

auto MusigCreatorModule::list() -> ReqId {
    return 0;
}

auto MusigCreatorModule::autoStart() -> ReqId {
    return 0;
}

void MusigCreatorModule::startInstance(const QString &id) {
    Q_UNUSED(id);
}

auto MusigCreatorModule::nextReqId() -> ReqId {
    return 0;
}
