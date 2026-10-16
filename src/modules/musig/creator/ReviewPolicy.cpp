#include "modules/musig/creator/ReviewPolicy.h"

#include "i18n/Tr.h"

namespace musig::modal {

namespace {

const QString K_NAME_SEPARATOR = QStringLiteral(", ");

auto signersWords(PolicySemantic semantic, std::uint32_t threshold, const QStringList &names,
                  const QList<bool> &mandatory) -> QString {
    auto count = static_cast<std::uint32_t>(names.size());
    switch (semantic) {
    case PolicySemantic::Single:
        return TR("musig-words-single").arg(names.value(0));
    case PolicySemantic::Multi:
        if (count == 1) {
            return TR("musig-words-single").arg(names.value(0));
        }
        if (threshold == count) {
            return TR("musig-words-all").arg(names.join(K_NAME_SEPARATOR));
        }
        if (threshold == 1) {
            return TR("musig-words-multi-one").arg(names.join(K_NAME_SEPARATOR));
        }
        return TR("musig-words-multi").arg(threshold).arg(names.join(K_NAME_SEPARATOR));
    case PolicySemantic::MultiMandatory: {
        // Split by index: two signers may share a display name.
        QStringList always;
        QStringList others;
        for (qsizetype i = 0; i < names.size(); ++i) {
            if (mandatory.value(i)) {
                always.append(names.at(i));
            } else {
                others.append(names.at(i));
            }
        }
        auto cosigners = static_cast<int>(threshold) - static_cast<int>(always.size());
        auto text =
            always.size() == 1 ? TR("musig-words-mandatory-one") : TR("musig-words-mandatory");
        return text.arg(always.join(K_NAME_SEPARATOR))
            .arg(cosigners)
            .arg(others.join(K_NAME_SEPARATOR));
    }
    case PolicySemantic::Other:
        break;
    }
    return TR("musig-words-other");
}

auto timelockWords(PolicyLocktime locktime, std::uint32_t locktime_value) -> QString {
    switch (locktime) {
    case PolicyLocktime::None:
        break;
    case PolicyLocktime::Relative:
        if (locktime_value == 1) {
            return TR("musig-words-after-blocks-one");
        }
        return TR("musig-words-after-blocks").arg(locktime_value);
    case PolicyLocktime::AbsoluteRenewable:
        return TR("musig-words-from-height-renewable").arg(locktime_value);
    case PolicyLocktime::Absolute:
        return TR("musig-words-from-height").arg(locktime_value);
    }
    return {};
}

} // namespace

auto pathWords(PolicySemantic semantic, std::uint32_t threshold, PolicyLocktime locktime,
               std::uint32_t locktime_value, const QStringList &names, const QList<bool> &mandatory)
    -> QString {
    auto signers = signersWords(semantic, threshold, names, mandatory);
    auto timelock = timelockWords(locktime, locktime_value);
    if (timelock.isEmpty()) {
        return signers;
    }
    return TR("musig-words-timelocked").arg(signers, timelock);
}

ReviewPolicy::ReviewPolicy(QWidget *parent) : QWidget(parent) {
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void ReviewPolicy::setReview(const PolicyReview &review, const QString &descriptor) {
    Q_UNUSED(review);
    Q_UNUSED(descriptor);
}

void ReviewPolicy::init() {
}

void ReviewPolicy::view() {
}

auto ReviewPolicy::pathText(const PolicyReviewPath &path) -> QString {
    Q_UNUSED(path);
    return {};
}

} // namespace musig::modal
