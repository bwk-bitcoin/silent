#include "modules/multikey/creator/MusigCreatorModule.h"

#include "AppController.h"
#include "account/services/ServiceAdapters.h"
#include "account/views/RestoreAccount.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "interfaces/host.h"
#include "modules/keys/DeviceName.h"
#include "modules/multikey/MusigModule.h"
#include "modules/multikey/creator/CreatePolicy.h"

#include <QList>
#include <QPair>
#include <QStringList>
#include <common.h>
#include <optional>

namespace {
const QString K_CREATE_INSTANCE_ID = QStringLiteral("musig.create");
const QString K_RESTORE_INSTANCE_ID = QStringLiteral("musig.restore");
MusigCreatorModule *gMusigCreatorModule = nullptr;
} // namespace

MusigCreatorModule::MusigCreatorModule() : IModule(nullptr) {
    gMusigCreatorModule = this;
}

MusigCreatorModule::~MusigCreatorModule() {
    if (gMusigCreatorModule == this) {
        gMusigCreatorModule = nullptr;
    }
}

auto MusigCreatorModule::get() -> MusigCreatorModule * {
    return gMusigCreatorModule;
}

auto MusigCreatorModule::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), QStringLiteral("musig.creator")},
            {QStringLiteral("name"), QStringLiteral("MuSig2 Account Creator")},
            {QStringLiteral("icon"), QStringLiteral("users")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

auto MusigCreatorModule::list() -> ReqId {
    ReqId reqId = nextReqId();
    QList<QPair<QString, QString>> instances{{K_CREATE_INSTANCE_ID, TR("musig-account-type")},
                                             {K_RESTORE_INSTANCE_ID, TR("musig-account-restore")}};
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
    if (id == K_RESTORE_INSTANCE_ID) {
        auto *target = account::restoreTarget(QString::fromLatin1(MUSIG_PLUGIN_ID), nullptr);
        connect(target, &backend::IAccountBackend::restored, this,
                &MusigCreatorModule::onAccountRestored, qontrol::UNIQUE);
        AppController::execModal(
            new account::view::RestoreAccount(target, this, &keys::deviceName));
        return;
    }
    emit error(std::nullopt, TR("musig-creator-unknown-instance").arg(id));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void MusigCreatorModule::onAccountRestored() {
    Host::get()->refreshLauncher();
}

auto MusigCreatorModule::nextReqId() -> ReqId {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}
