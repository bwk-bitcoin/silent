#pragma once

#include "interfaces/backend/provider.h"
#include "interfaces/backend/service.h"
#include "interfaces/backend/types.h"

#include <QObject>
#include <QString>

namespace host {

// The host's explicit implementation of a `kind` service no provider supplies: getters
// answer empty data (the account and network for identity) and every request answers
// error() inline.
auto unsupportedService(backend::ServiceKind kind, const QString &account, backend::Network network,
                        QObject *parent) -> backend::IService *;

} // namespace host
