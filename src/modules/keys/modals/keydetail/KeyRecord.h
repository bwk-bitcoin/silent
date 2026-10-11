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

// The key's record, none once it is removed.
auto keyInfo(KeysModule *module, const QString &fingerprint) -> std::optional<KeyInfo>;
auto keyAccounts(KeysModule *module, const QString &fingerprint) -> QList<KeyAccount>;
// The title on the left, the fingerprint and the key status on the right.
auto keyHeader(const QString &title, const QString &fingerprint, const KeyInfo &info) -> QWidget *;

} // namespace keys::modal
