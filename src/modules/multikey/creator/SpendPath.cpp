#include "modules/multikey/creator/SpendPath.h"

#include "i18n/Tr.h"

#include <QHash>
#include <QLocale>
#include <algorithm>

namespace musig::modal {

namespace {

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
    return static_cast<int>(::policy_threshold(picked, mandatory, std::max(path.threshold, 0)));
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

auto resolve(const SpendPath &path, Network network, std::optional<std::uint32_t> tip)
    -> PolicyDraftResult {
    PolicyDraft draft;
    for (const auto &key : path.keys) {
        draft.keys.push_back(rust::String(key.toStdString()));
    }
    for (const auto &key : path.mandatory) {
        draft.mandatory.push_back(rust::String(key.toStdString()));
    }
    draft.threshold = std::max(path.threshold, 0);
    draft.locktime = static_cast<PolicyLocktime>(path.lock);
    draft.count = std::max(path.count, 0);
    draft.unit = static_cast<PolicyDurationUnit>(path.unit);
    draft.date_timestamp = path.date.toSecsSinceEpoch();
    draft.date_valid = path.date.isValid();
    return ::resolve_policy_draft(draft, network, tip.has_value(), tip.value_or(0),
                                  QDateTime::currentSecsSinceEpoch());
}

// The height `path` opens at, counted from `tip`: a renewable one on bwk-bup's grid.
auto lockHeight(const SpendPath &path, std::uint32_t tip) -> std::uint32_t {
    return resolve(path, Network::Regtest, tip).path.locktime_value;
}

} // namespace

auto pathTitle(const SpendPath &path, int index) -> QString {
    auto name = path.name.trimmed();
    return name.isEmpty() ? TR("musig-create-path-title").arg(index + 1) : name;
}

auto unitBlocks(int count, LockUnit unit) -> std::uint64_t {
    return ::policy_duration_blocks(std::max(count, 0), static_cast<PolicyDurationUnit>(unit));
}

auto lockReason(const SpendPath &path, std::optional<std::uint32_t> tip) -> QString {
    auto result = resolve(path, Network::Regtest, tip);
    switch (result.lock_error) {
    case PolicyDraftError::None:
        return {};
    case PolicyDraftError::Count:
        return TR("musig-lock-reason-count");
    case PolicyDraftError::Date:
        return TR("musig-lock-reason-date");
    case PolicyDraftError::Future:
        return TR("musig-lock-reason-future");
    case PolicyDraftError::RelativeMax:
        return TR("musig-lock-reason-relative-max");
    case PolicyDraftError::Tip:
        return TR("musig-lock-reason-tip");
    default:
        return QString::fromStdString(std::string(result.error));
    }
}

auto policyPath(const SpendPath &path, Network network, std::optional<std::uint32_t> tip)
    -> PolicyPath {
    return resolve(path, network, tip).path;
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
