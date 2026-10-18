#pragma once

#include "interfaces/keys.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <silent.h>

namespace musig::modal {

// When a spend path opens: never locked, a count after each coin is received, a count
// from the chain tip (a renewable height), or a date and time.
enum class LockKind : std::uint8_t { None, Relative, AbsoluteRenewable, Date };
// The unit of a count; one block is ten minutes.
enum class LockUnit : std::uint8_t { Blocks, Days, Weeks, Months };

// One spend path as the user writes it: the keys picked, in the network's key order,
// those that sign every spend, the signatures needed (mandatory ones included), and
// when it opens. Turned into a PolicyPath, heights counted from the tip, by
// policyPath().
struct SpendPath {
    QString name;
    QStringList keys;
    QStringList mandatory;
    int threshold = 1;
    LockKind lock = LockKind::None;
    int count = 0;
    LockUnit unit = LockUnit::Days;
    QDateTime date;
};

// The name the user gave `path`, else "Spend Path `index` + 1".
[[nodiscard]] auto pathTitle(const SpendPath &path, int index) -> QString;
// The blocks `count` units make.
[[nodiscard]] auto unitBlocks(int count, LockUnit unit) -> std::uint32_t;
// Why the timelock of `path` cannot be compiled, empty when it can. A height needs the
// chain `tip`.
[[nodiscard]] auto lockReason(const SpendPath &path, std::optional<std::uint32_t> tip) -> QString;
// The path at BIP-87 account 0 of `network`, its heights counted from `tip`; every key
// Internal. Check lockReason() first.
[[nodiscard]] auto policyPath(const SpendPath &path, Network network,
                              std::optional<std::uint32_t> tip) -> PolicyPath;
// Who signs, in words: "Any 2 of Carol, Alice, Bob sign". `aliases` maps a fingerprint to
// the name it is shown by.
[[nodiscard]] auto signerWords(const SpendPath &path, const QHash<QString, QString> &aliases)
    -> QString;
// The timelock alone, as a sentence: "30 days after a coin is received".
[[nodiscard]] auto lockLine(const SpendPath &path, std::optional<std::uint32_t> tip) -> QString;
// What a count or date stands for: the time a block count lasts, the blocks a count of
// days makes, the height a renewable count reaches. Empty while nothing is entered.
[[nodiscard]] auto lockHint(const SpendPath &path, std::optional<std::uint32_t> tip) -> QString;

} // namespace musig::modal
