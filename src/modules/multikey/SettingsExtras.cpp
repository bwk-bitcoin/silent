#include "modules/multikey/SettingsExtras.h"

#include "AppController.h"
#include "account/InterfaceEnums.h"
#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/Section.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/multikey/MusigAccountController.h"
#include "modules/multikey/MusigModule.h"
#include "modules/multikey/RegisterDescriptor.h"
#include "theme/Palette.h"

#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <Qontrol>
#include <common.h>
#include <silent.h>
#include <string>

namespace musig {

namespace {

const QString K_NAME_SEPARATOR = QStringLiteral(", ");
const QString K_INDENT = QStringLiteral("  ");

// A key expression, [fingerprint/origin]xpub, its fingerprint captured.
const QRegularExpression
    K_KEY_EXPRESSION(QStringLiteral("\\[([0-9a-fA-F]{8})[^\\]]*\\][1-9A-HJ-NP-Za-km-z]+"));

auto toQString(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

auto keyName(const PolicyReviewKey &key, const QHash<QString, QString> &aliases) -> QString {
    auto fingerprint = toQString(key.fingerprint);
    return aliases.value(fingerprint.toLower(), fingerprint);
}

// Who signs `path`, in words: "Any 2 of Carol, Alice, Bob sign".
auto signerWords(const PolicyReviewPath &path, const QHash<QString, QString> &aliases) -> QString {
    QStringList names;
    QStringList always;
    QStringList others;
    for (const auto &key : path.keys) {
        auto name = keyName(key, aliases);
        names.append(name);
        if (key.mandatory) {
            always.append(name);
        } else {
            others.append(name);
        }
    }
    auto count = static_cast<std::uint32_t>(names.size());
    switch (path.semantic) {
    case PolicySemantic::Single:
        return TR("musig-words-single").arg(names.value(0));
    case PolicySemantic::Multi:
        if (count == 1) {
            return TR("musig-words-single").arg(names.value(0));
        }
        if (path.threshold == count) {
            return TR("musig-words-all").arg(names.join(K_NAME_SEPARATOR));
        }
        if (path.threshold == 1) {
            return TR("musig-words-multi-one").arg(names.join(K_NAME_SEPARATOR));
        }
        return TR("musig-words-multi").arg(path.threshold).arg(names.join(K_NAME_SEPARATOR));
    case PolicySemantic::MultiMandatory: {
        auto text =
            always.size() == 1 ? TR("musig-words-mandatory-one") : TR("musig-words-mandatory");
        return text.arg(always.join(K_NAME_SEPARATOR))
            .arg(static_cast<int>(path.threshold) - static_cast<int>(always.size()))
            .arg(others.join(K_NAME_SEPARATOR));
    }
    case PolicySemantic::Other:
        break;
    }
    return TR("musig-words-other");
}

// The timelock of `path` alone, as a sentence: "144 blocks after a coin is received".
auto lockLine(const PolicyReviewPath &path) -> QString {
    switch (path.locktime) {
    case PolicyLocktime::None:
        break;
    case PolicyLocktime::Relative:
        return TR("musig-lock-line-relative")
            .arg(path.locktime_value == 1 ? TR("musig-count-block")
                                          : TR("musig-count-blocks").arg(path.locktime_value));
    case PolicyLocktime::AbsoluteRenewable:
        return TR("musig-lock-line-renewable").arg(path.locktime_value);
    case PolicyLocktime::Absolute:
        return TR("musig-lock-line-absolute").arg(path.locktime_value);
    }
    return TR("musig-locktime-none");
}

// One path in words: its signers, its timelock, and its keys with the mandatory ones
// marked.
auto policyCard(const PolicyReviewPath &path, int index, const QHash<QString, QString> &aliases)
    -> QWidget * {
    auto *title = new catalog::Label(
        TR("musig-review-path")
            .arg(TR("musig-create-path-title").arg(index + 1), signerWords(path, aliases)),
        catalog::LabelRole::Subheading);
    title->setWordWrap(true);
    auto *lock = new catalog::Label(lockLine(path), catalog::LabelRole::Caption);
    lock->setTone(catalog::LabelTone::Secondary);
    auto *content = (new qontrol::Column)->spacing(resolve(Spacing::XXS))->push(title)->push(lock);
    for (const auto &key : path.keys) {
        auto *name = new catalog::Label(keyName(key, aliases));
        auto *fingerprint =
            new catalog::Label(toQString(key.fingerprint), catalog::LabelRole::Mono);
        fingerprint->setTone(catalog::LabelTone::Muted);
        auto *row = (new qontrol::Row)->spacing(resolve(Spacing::S))->push(name)->push(fingerprint);
        if (key.mandatory) {
            row->push(new catalog::Label(TR("musig-path-mandatory"), catalog::LabelRole::Caption));
        }
        content->push(row->pushSpacer());
    }
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setPad(catalog::Card::Pad::M);
    card->setContent(content);
    return card;
}

// `descriptor` with each known key read as its alias, indented by nesting: one key per
// line, each fragment's derivation suffix on its closing line, the checksum last.
auto prettyDescriptor(const QString &descriptor, const QHash<QString, QString> &aliases)
    -> QString {
    auto body = descriptor.section(QChar('#'), 0, 0);
    auto checksum = descriptor.section(QChar('#'), 1);

    QString named;
    qsizetype copied = 0;
    auto matches = K_KEY_EXPRESSION.globalMatch(body);
    while (matches.hasNext()) {
        auto match = matches.next();
        named += body.mid(copied, match.capturedStart() - copied);
        named += aliases.value(match.captured(1).toLower(), match.captured(0));
        copied = match.capturedEnd();
    }
    named += body.mid(copied);

    QString out;
    int depth = 0;
    auto newLine = [&depth]() -> QString { return QChar('\n') + K_INDENT.repeated(depth); };
    for (const auto &ch : named) {
        if (ch == QChar('(')) {
            ++depth;
            out += ch;
            out += newLine();
        } else if (ch == QChar(')')) {
            --depth;
            out += newLine();
            out += ch;
        } else if (ch == QChar(',')) {
            out += ch;
            out += newLine();
        } else {
            out += ch;
        }
    }
    if (!checksum.isEmpty()) {
        out += QStringLiteral("\n#") + checksum;
    }
    return out;
}

} // namespace

SettingsExtras::SettingsExtras(MusigAccountController *ctrl) : m_controller(ctrl) {
    init();
    doConnect();
    view();
}

void SettingsExtras::init() {
    auto descriptor = toQString(m_controller->getDescriptor());
    auto *keys = Host::get()->keys(MusigModule::get());
    auto metadata = keys != nullptr
                        ? keys->keys(account::toPublic<plugin::Network>(m_controller->network()))
                        : KeyListResult{.error = TR("musig-send-no-keys")};
    auto aliases = keyAliases(metadata.entries);

    m_policy = (new qontrol::Column)->spacing(resolve(Spacing::S));
    auto review = ::review_policy(rust::String(descriptor.toStdString()));
    if (!metadata.is_ok) {
        auto *error = new catalog::Label(metadata.error);
        error->setTone(catalog::LabelTone::Error);
        error->setWordWrap(true);
        m_policy->push(error);
    } else if (review.is_ok) {
        int index = 0;
        for (const auto &path : review.paths) {
            m_policy->push(policyCard(path, index++, aliases));
        }
    } else {
        auto *error = new catalog::Label(toQString(review.error));
        error->setTone(catalog::LabelTone::Error);
        error->setWordWrap(true);
        m_policy->push(error);
    }

    m_descriptor = new catalog::Label(catalog::LabelRole::Mono);
    m_descriptor->setTextFormat(Qt::PlainText);
    m_descriptor->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_descriptor->setTextWrapAnywhere(prettyDescriptor(descriptor, aliases));
    m_copy_btn = catalog::btnCopyValue();
    m_copy_btn->setTextToCopy(descriptor);

    m_register_btn = catalog::btnRegisterDescriptor();
    m_register_btn->setEnabled(metadata.is_ok);
}

void SettingsExtras::doConnect() {
    connect(m_register_btn, &QPushButton::clicked, this, &SettingsExtras::onRegisterClicked,
            qontrol::UNIQUE);
}

void SettingsExtras::view() {
    auto *registerRow = (new qontrol::Row)->pushSpacer()->push(m_register_btn);
    auto *policy = new catalog::Section(TR("musig-create-preview"));
    policy->setContent(
        (new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_policy)->push(registerRow));

    auto *descriptorContent = (new qontrol::Row)
                                  ->spacing(resolve(Spacing::S))
                                  ->push(m_descriptor, 1)
                                  ->pushAligned(m_copy_btn, Qt::AlignTop);
    auto *descriptorCard = new catalog::Card(catalog::Card::Role::Inset);
    descriptorCard->setPad(catalog::Card::Pad::M);
    descriptorCard->setContent(descriptorContent);
    auto *descriptor = new catalog::Section(TR("musig-settings-descriptor"));
    descriptor->setContent(descriptorCard);

    auto *col = (new qontrol::Column)->spacing(resolve(Spacing::L))->push(policy)->push(descriptor);
    setLayout(col->layout());
}

auto SettingsExtras::keyAliases(const QList<KeyEntry> &entries) -> QHash<QString, QString> {
    QHash<QString, QString> aliases;
    for (const auto &entry : entries) {
        if (!entry.alias.isEmpty()) {
            aliases.insert(entry.fingerprint.toLower(), entry.alias);
        }
    }
    return aliases;
}

// ===== Slots =====

void SettingsExtras::onRegisterClicked() {
    AppController::execModal(new modal::RegisterDescriptor(m_controller));
}

} // namespace musig
