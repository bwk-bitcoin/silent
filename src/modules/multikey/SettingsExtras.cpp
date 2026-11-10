#include "modules/multikey/SettingsExtras.h"

#include "AppController.h"
#include "account/InterfaceEnums.h"
#include "account/RequestId.h"
#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/Section.h"
#include "catalog/display/Label.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/inputs/ComboBox.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/multikey/MusigAccountController.h"
#include "modules/multikey/MusigModule.h"
#include "modules/multikey/RegisterDescriptor.h"
#include "theme/Palette.h"

#include <QIntValidator>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <Qontrol>
#include <common.h>
#include <limits>
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

    initBackend();
    initRescan();
}

void SettingsExtras::initBackend() {
    m_backend_text = new catalog::Label(TR("musig-settings-backend-text"));
    m_backend_text->setTone(catalog::LabelTone::Secondary);
    m_backend_text->setWordWrap(true);

    m_backend_picker = new catalog::ComboBox;
    m_backend_picker->setMinimumWidth(metric::SETTINGS_PICKER_WIDTH);
    m_backend_picker->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_backend_picker->addItem(TR("musig-backend-electrum"), QString());
    auto *registry = AppController::get()->pluginRegistry();
    auto backends = registry != nullptr ? registry->blockchainBackends()
                                        : QList<PluginRegistry::BlockchainBackend>();
    for (const auto &backend : backends) {
        m_backend_picker->addItem(backend.name, backend.moduleId);
    }
    auto chosen = m_controller->blockchainBackend();
    if (m_backend_picker->findData(chosen) < 0) {
        m_backend_picker->addItem(chosen, chosen);
    }

    m_backend_error = new catalog::Label;
    m_backend_error->setTone(catalog::LabelTone::Error);
    m_backend_error->setWordWrap(true);
    m_backend_error->setText(TR("musig-backend-missing").arg(chosen));
    m_backend_error->setVisible(m_controller->blockchainBackendMissing());
    showSavedBackend();
}

void SettingsExtras::showSavedBackend() {
    m_backend_picker->setCurrentIndex(
        m_backend_picker->findData(m_controller->blockchainBackend()));
}

// A Multikey account records no birthday height, so the field starts empty.
void SettingsExtras::initRescan() {
    m_backend = m_controller->blockchainBackendService();
    m_rescan_input = new catalog::ValidatedInput(catalog::InputRole::Mono);
    m_rescan_input->setWidth(Size::S);
    m_rescan_input->setFixedWidth(resolve(Size::S));
    m_rescan_input->input()->setPlaceholderText(TR("musig-rescan-placeholder"));
    m_rescan_input->input()->setValidator(
        new QIntValidator(0, std::numeric_limits<int>::max(), m_rescan_input));
    m_rescan_btn = catalog::btnRescan();
    m_rescan_btn->setEnabled(false);
    m_rescan_status = new catalog::Label;
    m_rescan_status->setWordWrap(true);
    m_rescan_status->setVisible(false);
    m_rescan =
        (new qontrol::Column)
            ->spacing(resolve(Spacing::S))
            ->push(new catalog::Label(TR("musig-rescan-label"), catalog::LabelRole::InputLabel))
            ->push((new qontrol::Row)
                       ->spacing(resolve(Spacing::S))
                       ->push(m_rescan_input)
                       ->push(m_rescan_btn)
                       ->pushSpacer())
            ->push(m_rescan_status);
    m_rescan->setVisible(m_backend != nullptr);
}

void SettingsExtras::doConnect() {
    connect(m_register_btn, &QPushButton::clicked, this, &SettingsExtras::onRegisterClicked,
            qontrol::UNIQUE);
    connect(m_backend_picker, qOverload<int>(&QComboBox::activated), this,
            &SettingsExtras::onBackendActivated, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::blockchainBackendFailed, this,
            &SettingsExtras::onBackendFailed, qontrol::UNIQUE);
    connect(m_rescan_input, &catalog::ValidatedInput::textChanged, this,
            &SettingsExtras::onRescanHeightChanged, qontrol::UNIQUE);
    connect(m_rescan_btn, &QPushButton::clicked, this, &SettingsExtras::onRescanClicked,
            qontrol::UNIQUE);
    if (m_backend != nullptr) {
        connect(m_backend.data(), &backend::IBlockchain::completed, this,
                &SettingsExtras::onRescanCompleted, qontrol::UNIQUE);
        connect(m_backend.data(), &backend::IBlockchain::error, this,
                &SettingsExtras::onRescanError, qontrol::UNIQUE);
    }
}

void SettingsExtras::view() {
    auto *backend = new catalog::Section(TR("musig-settings-backend"));
    backend->setContent((new qontrol::Column)
                            ->spacing(resolve(Spacing::S))
                            ->push(m_backend_text)
                            ->push((new qontrol::Row)->push(m_backend_picker)->pushSpacer())
                            ->push(m_backend_error)
                            ->push(m_rescan));

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

    auto *col = (new qontrol::Column)
                    ->spacing(resolve(Spacing::L))
                    ->push(backend)
                    ->push(policy)
                    ->push(descriptor);
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

// A saved choice rebuilds Settings over the new services.
void SettingsExtras::onBackendActivated(int index) {
    auto moduleId = m_backend_picker->itemData(index).toString();
    if (moduleId == m_controller->blockchainBackend()) {
        return;
    }
    m_backend_picker->setEnabled(false);
    m_backend_error->setVisible(false);
    m_controller->chooseBlockchainBackend(moduleId);
}

void SettingsExtras::onBackendFailed(const QString &message) {
    m_backend_picker->setEnabled(true);
    showSavedBackend();
    m_backend_error->setText(message);
    m_backend_error->setVisible(true);
}

void SettingsExtras::onRescanHeightChanged(const QString &text) {
    auto acceptable = m_rescan_input->input()->hasAcceptableInput();
    if (text.isEmpty()) {
        m_rescan_input->setState(catalog::ValidationMark::State::None);
    } else {
        m_rescan_input->setValid(acceptable);
    }
    m_rescan_btn->setEnabled(acceptable && m_rescan_req == 0);
}

// The backend may answer during the call: the request is recorded before it.
void SettingsExtras::onRescanClicked() {
    if (m_backend == nullptr || m_rescan_req != 0 ||
        !m_rescan_input->input()->hasAcceptableInput()) {
        return;
    }
    m_rescan_height = m_rescan_input->input()->text().toULongLong();
    m_rescan_req = account::nextRequestId();
    m_rescan_btn->setEnabled(false);
    m_rescan_status->setVisible(false);
    m_backend->rescan(m_rescan_req, m_rescan_height);
}

void SettingsExtras::onRescanCompleted(backend::RequestId request_id) {
    if (request_id == 0 || request_id != m_rescan_req) {
        return;
    }
    m_rescan_req = 0;
    m_rescan_status->setTone(catalog::LabelTone::Secondary);
    m_rescan_status->setText(TR("musig-rescan-started").arg(m_rescan_height));
    m_rescan_status->setVisible(true);
    onRescanHeightChanged(m_rescan_input->input()->text());
}

void SettingsExtras::onRescanError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0 || request_id != m_rescan_req) {
        return;
    }
    m_rescan_req = 0;
    m_rescan_status->setTone(catalog::LabelTone::Error);
    m_rescan_status->setText(message);
    m_rescan_status->setVisible(true);
    onRescanHeightChanged(m_rescan_input->input()->text());
}

} // namespace musig
