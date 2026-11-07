#pragma once

#include "interfaces/backend/provider.h"
#include "interfaces/backend/service.h"

class QObject;

namespace account {

class ServiceContext;

// The `kind` service of the account `context` serves, a child of `parent`. Each one is a
// separate QObject over the shared context: once the context is gone (the account
// closed), getters answer empty data and requests answer error(), pending ones
// included. Every getter and request is GUI thread only.
auto serviceAdapter(backend::ServiceKind kind, ServiceContext *context, QObject *parent)
    -> backend::IService *;

} // namespace account
