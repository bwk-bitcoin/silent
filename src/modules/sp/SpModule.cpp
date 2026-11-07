#include "SpModule.h"

#include "AccountController.h"
#include "AppController.h"
#include "SpInstance.h"
#include "account/services/ServiceAdapters.h"
#include "account/views/RestoreAccount.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "interfaces/host.h"
#include "modals/CreateAccount.h"
#include <QDebug>
#include <QStringList>
#include <common.h>
#include <memory>
#include <optional>
#include <silent.h>
#include <string>
#include <vector>

namespace {
const QString K_CREATE_INSTANCE_ID = QStringLiteral("sp.create");
const QString K_RESTORE_INSTANCE_ID = QStringLiteral("sp.restore");
SpModule *gSpModule = nullptr;
} // namespace

SpModule::SpModule() : AccountModule(SP_PLUGIN_ID, ::sp_account_delete_check, ::delete_sp_account) {
    gSpModule = this;
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this, &SpModule::onInstanceRemoved,
            qontrol::UNIQUE);
}

SpModule::~SpModule() {
    if (gSpModule == this) {
        gSpModule = nullptr;
    }
}

auto SpModule::get() -> SpModule * {
    return gSpModule;
}

QMap<QString, QString> SpModule::meta() const {
    return {{QStringLiteral("id"), QStringLiteral("sp.module")},
            {QStringLiteral("name"), QStringLiteral("Silent Payments Accounts")},
            {QStringLiteral("icon"), QStringLiteral("silent")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

bool SpModule::isSpAccount(const QString &account_id) {
    auto config = ::config_from_file(rust::String(account_id.toStdString()));
    if (!config->is_ok()) {
        qWarning() << "SpModule: skipping account" << account_id << "with invalid config:"
                   << QString::fromStdString(std::string(config->get_error().c_str()));
        return false;
    }
    return QString::fromStdString(std::string(config->get_plugin_id().c_str())) ==
           QString::fromLatin1(SP_PLUGIN_ID);
}

auto SpModule::instanceEntries(const QStringList &account_ids) const
    -> QList<QPair<QString, QString>> {
    auto entries = AccountModule::instanceEntries(account_ids);
    entries.append({K_CREATE_INSTANCE_ID, TR("sp-account-type")});
    entries.append({K_RESTORE_INSTANCE_ID, TR("sp-account-restore")});
    return entries;
}

ReqId SpModule::autoStart() {
    ReqId reqId = nextReqId();
    emit IModule::autoStart(reqId, QStringList{});
    return reqId;
}

void SpModule::startInstance(const QString &id) {
    if (accountUnavailable(id)) {
        emit error(std::nullopt, QStringLiteral("Account is open, closing or pending deletion"));
        return;
    }
    if (id == K_CREATE_INSTANCE_ID) {
        AppController::execModal(new modal::CreateAccount);
        return;
    }
    if (id == K_RESTORE_INSTANCE_ID) {
        auto *target = account::restoreTarget(QString::fromLatin1(SP_PLUGIN_ID), nullptr);
        connect(target, &backend::IAccountBackend::restored, this, &SpModule::onAccountRestored,
                qontrol::UNIQUE);
        AppController::execModal(new account::view::RestoreAccount(target));
        return;
    }

    if (!isSpAccount(id)) {
        emit error(std::nullopt, TR("sp-instance-not-found").arg(id));
        return;
    }

    auto instance = std::make_unique<SpInstance>(id);
    watchAccount(id, instance->backend());
    Host::get()->registerInstance(id, instance.get(), this);
    if (!instance->openPage()) {
        Host::get()->removeInstance(id, this);
        return;
    }
    m_instances.push_back(std::move(instance));
}

void SpModule::deleteInstance(const QString &id) {
    // AccountModule retains the closing backend's completion independently of the tab.
    std::erase_if(m_instances, [&id](const std::unique_ptr<IInstance> &instance) {
        return instance != nullptr && instance->id() == id;
    });
}

void SpModule::onInstanceRemoved(const QString &id) {
    deleteInstance(id);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void SpModule::onAccountRestored() {
    Host::get()->refreshLauncher();
}
