#include "modules/musig/creator/ReviewPolicy.h"

#include "catalog/containers/Card.h"
#include "catalog/display/CopyField.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <string>

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

auto sectionLabel(const QString &text) -> catalog::Label * {
    auto *label = new catalog::Label(text, catalog::LabelRole::Eyebrow);
    label->setTone(catalog::LabelTone::Muted);
    return label;
}

auto roleName(PolicyKeyType key_type) -> QString {
    switch (key_type) {
    case PolicyKeyType::Internal:
        return TR("musig-key-type-internal");
    case PolicyKeyType::External:
        return TR("musig-key-type-external");
    case PolicyKeyType::Cosigner:
        return TR("musig-key-type-cosigner");
    case PolicyKeyType::SafetyNet:
        return TR("musig-key-type-safety-net");
    case PolicyKeyType::Unknown:
        break;
    }
    return TR("musig-key-type-unknown");
}

// One signer of a path: its name and role, its origin, and whether it signs every spend.
auto keyRow(const QString &name, const PolicyReviewKey &key) -> QWidget * {
    auto origin = QString::fromStdString(std::string(key.origin_path));
    if (origin.startsWith(QStringLiteral("m/"))) {
        origin = origin.mid(2);
    }
    auto *nameLabel = new catalog::Label(name, catalog::LabelRole::Body);
    auto *roleLabel = new catalog::Label(roleName(key.key_type), catalog::LabelRole::Caption);
    roleLabel->setTone(catalog::LabelTone::Secondary);
    auto *originLabel = new catalog::Label(
        QStringLiteral("[%1/%2]").arg(QString::fromStdString(std::string(key.fingerprint)), origin),
        catalog::LabelRole::Mono);
    originLabel->setTone(catalog::LabelTone::Muted);
    auto *row = (new qontrol::Row)
                    ->spacing(resolve(Spacing::S))
                    ->push(nameLabel)
                    ->push(roleLabel)
                    ->push(originLabel);
    if (key.mandatory) {
        auto *mandatory =
            new catalog::Label(TR("musig-review-mandatory"), catalog::LabelRole::Caption);
        mandatory->setTone(catalog::LabelTone::Accent);
        row->push(mandatory);
    }
    row->pushSpacer();
    return row;
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
    init();
    view();
}

void ReviewPolicy::setAliases(const QHash<QString, QString> &aliases) {
    m_aliases = aliases;
}

void ReviewPolicy::setReview(const PolicyReview &review, const QString &descriptor) {
    m_paths_column->clear();
    m_paths_column->spacing(resolve(Spacing::S));
    for (std::size_t i = 0; i < review.paths.size(); ++i) {
        auto *card = pathCard(static_cast<int>(i), review.paths[i]);
        m_paths_column->push(card);
        // A card added to a shown column stays hidden until the event loop shows it.
        card->show();
    }
    m_leaves_label->setText(TR("musig-review-total-leaves").arg(review.leaf_count));

    auto checksumAt = descriptor.lastIndexOf(QLatin1Char('#'));
    m_descriptor_field->setValue(checksumAt < 0 ? descriptor : descriptor.left(checksumAt));
    m_checksum_label->setText(checksumAt < 0 ? QString() : descriptor.mid(checksumAt + 1));
}

void ReviewPolicy::init() {
    m_paths_column = new qontrol::Column;
    m_leaves_label = new catalog::Label(catalog::LabelRole::Body);
    m_descriptor_field = new catalog::CopyField;
    m_descriptor_field->setWrap(true);
    m_checksum_label = new catalog::Label(catalog::LabelRole::Mono);
}

void ReviewPolicy::view() {
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(sectionLabel(TR("musig-create-preview")))
        ->push(m_paths_column)
        ->push(m_leaves_label)
        ->push(sectionLabel(TR("musig-settings-descriptor")))
        ->push(m_descriptor_field)
        ->push(sectionLabel(TR("musig-review-checksum")))
        ->push(m_checksum_label)
        ->into(this);
}

auto ReviewPolicy::pathText(const PolicyReviewPath &path) const -> QString {
    QStringList names;
    QList<bool> mandatory;
    for (const auto &key : path.keys) {
        names.append(keyName(QString::fromStdString(std::string(key.fingerprint))));
        mandatory.append(key.mandatory);
    }
    return pathWords(path.semantic, path.threshold, path.locktime, path.locktime_value, names,
                     mandatory);
}

auto ReviewPolicy::keyName(const QString &fingerprint) const -> QString {
    auto alias = m_aliases.value(fingerprint);
    return alias.isEmpty() ? fingerprint : alias;
}

auto ReviewPolicy::pathCard(int index, const PolicyReviewPath &path) const -> QWidget * {
    auto *title =
        new catalog::Label(TR("musig-create-path-words").arg(index + 1).arg(pathText(path)),
                           catalog::LabelRole::Subheading);
    title->setWordWrap(true);
    auto *leaves =
        new catalog::Label(path.key_path ? TR("musig-review-key-path")
                                         : TR("musig-review-leaves").arg(path.leaf_count),
                           catalog::LabelRole::Caption);
    leaves->setTone(catalog::LabelTone::Secondary);
    auto *content = (new qontrol::Column)->spacing(resolve(Spacing::XS))->push(title)->push(leaves);
    for (const auto &key : path.keys) {
        content->push(keyRow(keyName(QString::fromStdString(std::string(key.fingerprint))), key));
    }
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setContent(content);
    return card;
}

} // namespace musig::modal
