#include "modules/musig/MusigModule.h"

#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigInstance.h"

#include <QDebug>
#include <QList>
#include <QPair>
#include <QStringList>
#include <Qontrol>
#include <common.h>
#include <optional>
#include <silent.h>
#include <string>
#include <utility>

namespace {
MusigModule *gMusigModule = nullptr;
} // namespace

MusigModule::MusigModule() : AccountModule(::musig_account_delete_check, ::delete_musig_account) {
    gMusigModule = this;
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &MusigModule::onInstanceRemoved, qontrol::UNIQUE);
}

MusigModule::~MusigModule() {
    if (gMusigModule == this) {
        gMusigModule = nullptr;
    }
}

auto MusigModule::get() -> MusigModule * {
    return gMusigModule;
}

auto MusigModule::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), QStringLiteral("musig.module")},
            {QStringLiteral("name"), QStringLiteral("MuSig2 Accounts")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

auto MusigModule::list() -> ReqId {
    ReqId reqId = nextReqId();
    QList<QPair<QString, QString>> instances;
    auto configs = ::list_module_configs(MUSIG_PLUGIN_ID);
    for (auto &name : configs) {
        QString accountId = QString::fromStdString(std::string(name.c_str()));
        instances.append({accountId, accountId});
    }
    emit IModule::instances(reqId, instances);
    return reqId;
}

auto MusigModule::autoStart() -> ReqId {
    ReqId reqId = nextReqId();
    emit IModule::autoStart(reqId, QStringList{});
    return reqId;
}

void MusigModule::startInstance(const QString &id) {
    if (accountUnavailable(id)) {
        emit error(std::nullopt, QStringLiteral("Account is open, closing or pending deletion"));
        return;
    }
    if (!isMusigAccount(id)) {
        emit error(std::nullopt, TR("musig-instance-not-found").arg(id));
        return;
    }

    auto instance = std::make_unique<MusigInstance>(id);
    watchAccount(id, instance->backend());
    Host::get()->registerInstance(id, instance.get(), this);
    if (!instance->openPage()) {
        Host::get()->removeInstance(id, this);
        return;
    }
    m_instances.push_back(std::move(instance));
}

void MusigModule::deleteInstance(const QString &id) {
    // AccountModule retains the closing backend's completion independently of the tab.
    std::erase_if(m_instances, [&id](const std::unique_ptr<IInstance> &instance) {
        return instance != nullptr && instance->id() == id;
    });
}

void MusigModule::onInstanceRemoved(const QString &id) {
    deleteInstance(id);
}

auto MusigModule::isMusigAccount(const QString &account_id) -> bool {
    auto config = ::config_from_file(rust::String(account_id.toStdString()));
    if (!config->is_ok()) {
        qWarning() << "MusigModule: skipping account" << account_id << "with invalid config:"
                   << QString::fromStdString(std::string(config->get_error().c_str()));
        return false;
    }
    return QString::fromStdString(std::string(config->get_plugin_id().c_str())) ==
           QString::fromLatin1(MUSIG_PLUGIN_ID);
}

auto MusigModule::nextReqId() -> ReqId {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}
