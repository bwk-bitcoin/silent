#pragma once

#include <optional>

#include <QList>
#include <QString>

#include <silent.h>

class QWidget;

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

// What KeyDetail and its flows read off the key record, and the header they show it
// under.

// An account whose descriptor this key signs for.
struct KeyAccount {
    QString name;
    QString descriptor;
};

// A key's xpub that an account's taproot or segwit sub-account is built on, as the
// account's config holds it: the path without "m/", hardened steps marked '.
struct AccountXpub {
    QString path;
    QString xpub;
};

// The key's record, none once it is removed.
auto keyInfo(KeysModule *module, const QString &fingerprint) -> std::optional<KeyInfo>;
auto keyAccounts(KeysModule *module, const QString &fingerprint) -> QList<KeyAccount>;
// Whether the key can be reached over `kind`.
auto hasRoute(const KeyInfo &info, KeyRouteKind kind) -> bool;
// The key's xpubs held by the taproot and segwit sub-accounts of the accounts on
// `network`, read from their configs: an account holds them whether or not the key
// still caches them, or has their descriptors registered.
auto accountXpubs(const QString &fingerprint, Network network) -> QList<AccountXpub>;
// The title on the left, the fingerprint and the key status on the right.
auto keyHeader(const QString &title, const QString &fingerprint, const KeyInfo &info) -> QWidget *;

} // namespace keys::modal
