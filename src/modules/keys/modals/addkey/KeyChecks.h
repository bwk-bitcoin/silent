#pragma once

#include <QList>
#include <QString>

#include "modules/keys/KeysModule.h"

namespace keys::modal {

// Why `alias` cannot name a new key, empty when it can.
[[nodiscard]] auto aliasReason(const KeysModule *module, const QString &alias) -> QString;
// The alias of the key already registered with `fingerprint`, empty when none is.
[[nodiscard]] auto registeredAlias(const KeysModule *module, const QString &fingerprint) -> QString;
// Adds the key: empty once added, else why the module refused it.
[[nodiscard]] auto addKey(KeysModule *module, const QString &fingerprint, const QString &alias,
                          const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs)
    -> QString;

} // namespace keys::modal
