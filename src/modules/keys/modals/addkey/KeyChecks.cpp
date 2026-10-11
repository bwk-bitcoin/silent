#include "KeyChecks.h"

#include "i18n/Tr.h"

namespace keys::modal {

auto aliasReason(const KeysModule *module, const QString &alias) -> QString {
    if (alias.isEmpty()) {
        return TR("keys-alias-empty");
    }
    for (const auto &info : module->keys()) {
        if (KeysModule::toQt(info.alias) == alias) {
            return TR("keys-alias-taken-reason");
        }
    }
    return QString();
}

auto registeredAlias(const KeysModule *module, const QString &fingerprint) -> QString {
    if (fingerprint.isEmpty()) {
        return QString();
    }
    for (const auto &info : module->keys()) {
        if (QString::compare(KeysModule::toQt(info.fingerprint), fingerprint,
                             Qt::CaseInsensitive) == 0) {
            return KeysModule::toQt(info.alias);
        }
    }
    return QString();
}

auto addKey(KeysModule *module, const QString &fingerprint, const QString &alias,
            const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs) -> QString {
    auto result = module->addKey(fingerprint, alias, routes, xpubs);
    if (!result.is_ok) {
        return TR("keys-add-failed").arg(KeysModule::toQt(result.error));
    }
    return QString();
}

} // namespace keys::modal
