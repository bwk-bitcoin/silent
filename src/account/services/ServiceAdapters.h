#pragma once

#include "interfaces/backend/provider.h"
#include "interfaces/backend/service.h"

class QObject;
class QString;

namespace backend {
class IAccountBackend;
} // namespace backend

namespace account {

class ServiceContext;

// The `kind` service of the account `context` serves, a child of `parent`. Each one is a
// separate QObject over the shared context: once the context is gone (the account
// closed), getters answer empty data and requests answer error(), pending ones
// included. Every getter and request is GUI thread only.
auto serviceAdapter(backend::ServiceKind kind, ServiceContext *context, QObject *parent)
    -> backend::IService *;
// The restore target of the built-in module `plugin_id` ("sp", "musig"), a child of
// `parent`: no account behind it, its inspectBackup() reads a backup of that module's
// accounts and its restore() makes the account it holds. Every other request answers
// error().
auto restoreTarget(const QString &plugin_id, QObject *parent) -> backend::IAccountBackend *;

} // namespace account
