#include "SpModule.h"

#include "AppController.h"
#include "SpInstance.h"
#include "modals/CreateAccount.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
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
} // namespace

SpModule::SpModule() : IModule(nullptr) {
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &SpModule::onInstanceRemoved, qontrol::UNIQUE);
}

QMap<QString, QString> SpModule::meta() const {
    return {{QStringLiteral("id"), QStringLiteral("sp.module")},
            {QStringLiteral("name"), QStringLiteral("Silent Payments Accounts")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

bool SpModule::isSpAccount(const QString &account_id) {
    auto config = ::config_from_file(rust::String(account_id.toStdString()));
    if (!config->is_ok()) {
        qWarning() << "SpModule: skipping account" << account_id
                   << "with invalid config:"
                   << QString::fromStdString(std::string(config->get_error().c_str()));
        return false;
    }
    return QString::fromStdString(std::string(config->get_plugin_id().c_str()))
           == QString::fromLatin1(SP_PLUGIN_ID);
}

ReqId SpModule::list() {
    ReqId reqId = nextReqId();
    QList<QPair<QString, QString>> instances;
    auto configs = ::list_configs();
    for (auto &name : configs) {
        QString accountId = QString::fromStdString(std::string(name.c_str()));
        if (!isSpAccount(accountId)) {
            continue;
        }
        instances.append({accountId, accountId});
    }

    instances.append({K_CREATE_INSTANCE_ID, TR("sp-create-account")});
    emit IModule::instances(reqId, instances);
    return reqId;
}

ReqId SpModule::autoStart() {
    ReqId reqId = nextReqId();
    emit IModule::autoStart(reqId, QStringList{});
    return reqId;
}

void SpModule::startInstance(const QString &id) {
    if (id == K_CREATE_INSTANCE_ID) {
        AppController::execModal(new modal::CreateAccount);
        return;
    }

    if (!isSpAccount(id)) {
        emit error(std::nullopt, TR("sp-instance-not-found").arg(id));
        return;
    }

    auto instance = std::make_unique<SpInstance>(id);
    Host::get()->registerInstance(id, instance.get(), this);
    m_instances.push_back(std::move(instance));
}

void SpModule::deleteInstance(const QString &id) {
    // The host owns the tab UI and SpInstance::stop already closed its tab, so
    // the instance is fully stopped here. Erase the owning unique_ptr.
    std::erase_if(m_instances, [&id](const std::unique_ptr<IInstance> &instance) {
        return instance != nullptr && instance->id() == id;
    });
}

void SpModule::onInstanceRemoved(const QString &id) {
    deleteInstance(id);
}

ReqId SpModule::nextReqId() {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}
