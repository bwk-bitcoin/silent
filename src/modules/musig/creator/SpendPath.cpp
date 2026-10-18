#include "modules/musig/creator/SpendPath.h"

#include "i18n/Tr.h"

#include <QHash>
#include <QLocale>
#include <algorithm>

namespace musig::modal {

namespace {

// The creator always uses BIP-87 account 0' (account choice is a later feature).
constexpr std::uint32_t BIP87_ACCOUNT = 0;
// An older() value is a 16-bit block count.
constexpr std::uint32_t MAX_RELATIVE_BLOCKS = 65535;
constexpr std::int64_t SECONDS_PER_BLOCK = 600;
constexpr std::uint32_t BLOCKS_PER_DAY = 144;
constexpr std::uint32_t BLOCKS_PER_WEEK = 1008;
constexpr std::uint32_t BLOCKS_PER_MONTH = 4320;
constexpr int BLOCKS_PER_HOUR = 6;
constexpr int HOURS_SHOWN_AS_HOURS = 48;
constexpr int DAYS_SHOWN_AS_DAYS = 60;
constexpr int DAYS_PER_MONTH = 30;
constexpr int HOURS_PER_DAY = 24;

const QString K_NAME_SEPARATOR = QStringLiteral(", ");

auto names(const QStringList &fingerprints, const QHash<QString, QString> &aliases) -> QString {
    QStringList out;
    for (const auto &fingerprint : fingerprints) {
        out.append(aliases.value(fingerprint, fingerprint));
    }
    return out.join(K_NAME_SEPARATOR);
}

// The threshold within [mandatory + 1, picked].
auto clampedThreshold(const SpendPath &path) -> int {
    auto picked = static_cast<int>(path.keys.size());
    auto mandatory = static_cast<int>(path.mandatory.size());
    return std::clamp(path.threshold, std::min(mandatory + 1, std::max(picked, 1)),
                      std::max(picked, 1));
}

// "30 days", "1 day".
auto countWords(int count, LockUnit unit) -> QString {
    bool one = count == 1;
    switch (unit) {
    case LockUnit::Blocks:
        return one ? TR("musig-count-block") : TR("musig-count-blocks").arg(count);
    case LockUnit::Days:
        return one ? TR("musig-count-day") : TR("musig-count-days").arg(count);
    case LockUnit::Weeks:
        return one ? TR("musig-count-week") : TR("musig-count-weeks").arg(count);
    case LockUnit::Months:
        return one ? TR("musig-count-month") : TR("musig-count-months").arg(count);
    }
    return {};
}

// About how long `blocks` last, at ten minutes a block.
auto blocksSpan(std::uint32_t blocks) -> QString {
    int hours = static_cast<int>(blocks) / BLOCKS_PER_HOUR;
    if (hours < HOURS_SHOWN_AS_HOURS) {
        return TR("musig-span-hours").arg(hours);
    }
    int days = hours / HOURS_PER_DAY;
    if (days < DAYS_SHOWN_AS_DAYS) {
        return TR("musig-span-days").arg(days);
    }
    return TR("musig-span-months").arg(days / DAYS_PER_MONTH);
}

// The blocks from now to `date`, rounded up; 0 for a date not in the future.
auto blocksUntil(const QDateTime &date) -> std::uint32_t {
    auto seconds = QDateTime::currentDateTime().secsTo(date);
    if (seconds <= 0) {
        return 0;
    }
    return static_cast<std::uint32_t>((seconds + SECONDS_PER_BLOCK - 1) / SECONDS_PER_BLOCK);
}

// The height `path` opens at, counted from `tip`: a renewable one on bwk-bup's grid.
auto lockHeight(const SpendPath &path, std::uint32_t tip) -> std::uint32_t {
    if (path.lock == LockKind::Date) {
        return tip + blocksUntil(path.date);
    }
    return ::renewable_height(tip + unitBlocks(path.count, path.unit));
}

} // namespace

auto pathTitle(const SpendPath &path, int index) -> QString {
    auto name = path.name.trimmed();
    return name.isEmpty() ? TR("musig-create-path-title").arg(index + 1) : name;
}

auto unitBlocks(int count, LockUnit unit) -> std::uint32_t {
    auto blocks = static_cast<std::uint32_t>(std::max(count, 0));
    switch (unit) {
    case LockUnit::Blocks:
        return blocks;
    case LockUnit::Days:
        return blocks * BLOCKS_PER_DAY;
    case LockUnit::Weeks:
        return blocks * BLOCKS_PER_WEEK;
    case LockUnit::Months:
        return blocks * BLOCKS_PER_MONTH;
    }
    return blocks;
}

auto lockReason(const SpendPath &path, std::optional<std::uint32_t> tip) -> QString {
    switch (path.lock) {
    case LockKind::None:
        return {};
    case LockKind::Relative:
        if (path.count <= 0) {
            return TR("musig-lock-reason-count");
        }
        if (unitBlocks(path.count, path.unit) > MAX_RELATIVE_BLOCKS) {
            return TR("musig-lock-reason-relative-max");
        }
        return {};
    case LockKind::AbsoluteRenewable:
        if (path.count <= 0) {
            return TR("musig-lock-reason-count");
        }
        break;
    case LockKind::Date:
        if (!path.date.isValid()) {
            return TR("musig-lock-reason-date");
        }
        if (blocksUntil(path.date) == 0) {
            return TR("musig-lock-reason-future");
        }
        break;
    }
    // A height is counted from the tip.
    if (!tip.has_value()) {
        return TR("musig-lock-reason-tip");
    }
    return {};
}

auto policyPath(const SpendPath &path, Network network, std::optional<std::uint32_t> tip)
    -> PolicyPath {
    PolicyPath out;
    auto picked = path.keys.size();
    out.threshold = static_cast<std::uint32_t>(clampedThreshold(path));
    if (picked == 1) {
        out.semantic = PolicySemantic::Single;
        out.threshold = 1;
    } else if (path.mandatory.isEmpty()) {
        out.semantic = PolicySemantic::Multi;
    } else {
        out.semantic = PolicySemantic::MultiMandatory;
    }
    switch (path.lock) {
    case LockKind::None:
        out.locktime = PolicyLocktime::None;
        out.locktime_value = 0;
        break;
    case LockKind::Relative:
        out.locktime = PolicyLocktime::Relative;
        out.locktime_value = unitBlocks(path.count, path.unit);
        break;
    case LockKind::AbsoluteRenewable:
        out.locktime = PolicyLocktime::AbsoluteRenewable;
        out.locktime_value = lockHeight(path, tip.value_or(0));
        break;
    case LockKind::Date:
        // A plain height: a date never renews.
        out.locktime = PolicyLocktime::Absolute;
        out.locktime_value = lockHeight(path, tip.value_or(0));
        break;
    }
    auto accountPath = ::bip87_account_path(network, BIP87_ACCOUNT);
    for (const auto &fingerprint : path.keys) {
        PolicyKey key;
        key.fingerprint = rust::String(fingerprint.toStdString());
        key.account_path = accountPath;
        key.key_type = PolicyKeyType::Internal;
        key.mandatory =
            out.semantic == PolicySemantic::MultiMandatory && path.mandatory.contains(fingerprint);
        out.keys.push_back(key);
    }
    return out;
}

auto signerWords(const SpendPath &path, const QHash<QString, QString> &aliases) -> QString {
    auto picked = path.keys.size();
    if (picked == 0) {
        return TR("musig-path-no-key");
    }
    if (picked == 1) {
        return TR("musig-words-single").arg(names(path.keys, aliases));
    }
    int threshold = clampedThreshold(path);
    if (threshold >= picked) {
        return TR("musig-words-all").arg(names(path.keys, aliases));
    }
    if (path.mandatory.isEmpty()) {
        if (threshold == 1) {
            return TR("musig-words-multi-one").arg(names(path.keys, aliases));
        }
        return TR("musig-words-multi").arg(threshold).arg(names(path.keys, aliases));
    }
    QStringList others;
    for (const auto &fingerprint : path.keys) {
        if (!path.mandatory.contains(fingerprint)) {
            others.append(fingerprint);
        }
    }
    auto text =
        path.mandatory.size() == 1 ? TR("musig-words-mandatory-one") : TR("musig-words-mandatory");
    return text.arg(names(path.mandatory, aliases))
        .arg(threshold - static_cast<int>(path.mandatory.size()))
        .arg(names(others, aliases));
}

auto lockLine(const SpendPath &path, std::optional<std::uint32_t> tip) -> QString {
    if (path.lock == LockKind::None) {
        return TR("musig-locktime-none");
    }
    if (!lockReason(path, tip).isEmpty()) {
        return TR("musig-lock-line-unset");
    }
    switch (path.lock) {
    case LockKind::None:
        break;
    case LockKind::Relative:
        return TR("musig-lock-line-relative").arg(countWords(path.count, path.unit));
    case LockKind::AbsoluteRenewable:
        return TR("musig-lock-line-renewable").arg(lockHeight(path, tip.value_or(0)));
    case LockKind::Date:
        return TR("musig-lock-line-date")
            .arg(QLocale().toString(path.date, QStringLiteral("d MMM yyyy, HH:mm")));
    }
    return {};
}

auto lockHint(const SpendPath &path, std::optional<std::uint32_t> tip) -> QString {
    bool counted = path.lock == LockKind::Relative || path.lock == LockKind::AbsoluteRenewable;
    if (!counted || path.count <= 0) {
        return {};
    }
    if (path.lock == LockKind::AbsoluteRenewable) {
        return tip.has_value() ? TR("musig-lock-hint-height").arg(lockHeight(path, tip.value()))
                               : QString();
    }
    if (path.unit == LockUnit::Blocks) {
        return blocksSpan(static_cast<std::uint32_t>(path.count));
    }
    return TR("musig-count-blocks").arg(unitBlocks(path.count, path.unit));
}

} // namespace musig::modal
