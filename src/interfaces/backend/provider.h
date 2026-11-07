#pragma once

#include <QString>
#include <QtGlobal>

#include "service.h"
#include "types.h"

namespace backend {

enum class ServiceKind : quint8 { Account, Blockchain, Transaction, Labels, Settings };

// Optional facet of an IModule, found with qobject_cast<backend::IServiceProvider *>: a
// module declaring it supplies some of an account UI's services. The host asks each
// service from the provider it was assigned to, so one account UI can use services of
// several plugins. GUI thread only; neither call performs I/O.
class IServiceProvider {
public:
    virtual ~IServiceProvider() = default;

    // This provider's `kind` service for `account` on `network`, or null when it supplies
    // none. The object declares Q_INTERFACES for the interface of `kind`, belongs to the
    // provider, and stays valid until it is handed back to releaseService().
    [[nodiscard]] virtual auto service(const QString &account, Network network, ServiceKind kind)
        -> IService * = 0;

    // Hands back one non-null service() answer: the host holds no reference to it any
    // more. The plugin cannot stop while a service it supplied is not handed back.
    virtual void releaseService(IService *service) = 0;
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IServiceProvider, "dev.silent.backend.IServiceProvider/1.0")
