#include "modules/musig/MusigModule.h"

MusigModule::MusigModule() : IModule(nullptr) {
}

auto MusigModule::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), QStringLiteral("musig.module")},
            {QStringLiteral("name"), QStringLiteral("MuSig2 Accounts")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

auto MusigModule::list() -> ReqId {
    return 0;
}

auto MusigModule::autoStart() -> ReqId {
    return 0;
}

void MusigModule::startInstance(const QString &id) {
    Q_UNUSED(id);
}

void MusigModule::deleteInstance(const QString &id) {
    Q_UNUSED(id);
}

void MusigModule::onInstanceRemoved(const QString &id) {
    Q_UNUSED(id);
}

auto MusigModule::isMusigAccount(const QString &account_id) -> bool {
    Q_UNUSED(account_id);
    return false;
}

auto MusigModule::nextReqId() -> ReqId {
    return 0;
}
