#include "modules/musig/creator/PathEditor.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QIntValidator>
#include <Qontrol>
#include <array>
#include <common.h>
#include <cstdint>
#include <utility>

namespace musig::modal {

namespace {

// The creator always uses BIP-87 account 0' (account choice is a later feature).
constexpr std::uint32_t BIP87_ACCOUNT = 0;
// An older() value is a 16-bit block count.
constexpr int MAX_RELATIVE_BLOCKS = 65535;
// An after() value at or above 500000000 is a timestamp, not a height.
constexpr int MAX_ABSOLUTE_HEIGHT = 499999999;

auto semanticItems() -> std::array<std::pair<QString, PolicySemantic>, 3> {
    return {{
        {TR("musig-shape-single"), PolicySemantic::Single},
        {TR("musig-shape-multi"), PolicySemantic::Multi},
        {TR("musig-shape-mandatory"), PolicySemantic::MultiMandatory},
    }};
}

auto locktimeItems() -> std::array<std::pair<QString, PolicyLocktime>, 4> {
    return {{
        {TR("musig-locktime-none"), PolicyLocktime::None},
        {TR("musig-locktime-relative"), PolicyLocktime::Relative},
        {TR("musig-locktime-renewable"), PolicyLocktime::AbsoluteRenewable},
        {TR("musig-locktime-absolute"), PolicyLocktime::Absolute},
    }};
}

auto keyTypeItems() -> std::array<std::pair<QString, PolicyKeyType>, 4> {
    return {{
        {TR("musig-key-type-internal"), PolicyKeyType::Internal},
        {TR("musig-key-type-external"), PolicyKeyType::External},
        {TR("musig-key-type-cosigner"), PolicyKeyType::Cosigner},
        {TR("musig-key-type-safety-net"), PolicyKeyType::SafetyNet},
    }};
}

} // namespace

PathEditor::PathEditor(const QList<KeyEntry> &keys, QWidget *parent)
    : QWidget(parent),
      m_keys(keys) {
    init();
    doConnect();
    view();
    applySemantic();
    refreshThresholds();
}

auto PathEditor::path(Network network) const -> PolicyPath {
    PolicyPath out;
    out.semantic = semantic();
    out.threshold =
        out.semantic == PolicySemantic::Single ? 1 : m_threshold_combo->currentData().toUInt();
    out.locktime = locktime();
    out.locktime_value =
        out.locktime == PolicyLocktime::None ? 0 : m_locktime_input->text().toUInt();
    auto accountPath = ::bip87_account_path(network, BIP87_ACCOUNT);
    for (int i = 0; i < m_keys.size(); ++i) {
        if (!m_key_checks.at(i)->isChecked()) {
            continue;
        }
        PolicyKey key;
        key.fingerprint = rust::String(m_keys.at(i).fingerprint.toStdString());
        key.account_path = accountPath;
        key.key_type = static_cast<PolicyKeyType>(m_key_type_combos.at(i)->currentData().toInt());
        key.mandatory =
            out.semantic == PolicySemantic::MultiMandatory && m_mandatory_checks.at(i)->isChecked();
        out.keys.push_back(key);
    }
    return out;
}

void PathEditor::setTitle(const QString &title) {
    m_title_label->setText(title);
}

void PathEditor::setRemovable(bool removable) {
    m_remove_btn->setVisible(removable);
}

void PathEditor::init() {
    m_title_label = new catalog::Label(catalog::LabelRole::Heading);
    m_remove_btn = catalog::btnRemovePath();
    m_remove_btn->setVisible(false);

    m_semantic_combo = new catalog::ComboBox;
    m_semantic_combo->setWidth(Size::M);
    for (const auto &[label, semantic] : semanticItems()) {
        m_semantic_combo->addItem(label, static_cast<int>(semantic));
    }
    m_threshold_combo = new catalog::ComboBox;
    m_threshold_combo->setWidth(Size::S);

    m_locktime_combo = new catalog::ComboBox;
    m_locktime_combo->setWidth(Size::L);
    for (const auto &[label, locktime] : locktimeItems()) {
        m_locktime_combo->addItem(label, static_cast<int>(locktime));
    }
    m_locktime_input = new catalog::Input(catalog::InputRole::Mono);
    m_locktime_input->setWidth(Size::XS);
    m_locktime_validator = new QIntValidator(1, MAX_RELATIVE_BLOCKS, m_locktime_input);
    m_locktime_input->setValidator(m_locktime_validator);
    m_locktime_input->setVisible(false);

    m_keys_column = new qontrol::Column;
    m_keys_column->spacing(resolve(Spacing::XS));
    for (const auto &entry : m_keys) {
        auto *check =
            new catalog::Checkbox(entry.alias.isEmpty() ? entry.fingerprint : entry.alias);
        auto *fingerprint = new catalog::Label(entry.fingerprint, catalog::LabelRole::Mono);
        fingerprint->setTone(catalog::LabelTone::Muted);
        fingerprint->setVisible(!entry.alias.isEmpty());

        auto *type = new catalog::ComboBox;
        type->setWidth(Size::S);
        for (const auto &[label, keyType] : keyTypeItems()) {
            type->addItem(label, static_cast<int>(keyType));
        }
        type->setEnabled(false);

        auto *mandatory = new catalog::Checkbox(TR("musig-create-signs-every-spend"));
        mandatory->setEnabled(false);

        m_key_checks.append(check);
        m_key_type_combos.append(type);
        m_mandatory_checks.append(mandatory);
        m_keys_column->push((new qontrol::Row)
                                ->spacing(resolve(Spacing::S))
                                ->push(check)
                                ->push(fingerprint)
                                ->pushSpacer()
                                ->push(mandatory)
                                ->push(type));
    }
    if (m_keys.isEmpty()) {
        m_keys_column->push(new catalog::EmptyState(icon::key, TR("create-account-no-key-title"),
                                                    TR("create-account-no-key-sub")));
    }
}

void PathEditor::doConnect() {
    connect(m_semantic_combo, &QComboBox::activated, this, &PathEditor::onSemanticChanged,
            qontrol::UNIQUE);
    connect(m_threshold_combo, &QComboBox::activated, this, &PathEditor::onThresholdChanged,
            qontrol::UNIQUE);
    connect(m_locktime_combo, &QComboBox::activated, this, &PathEditor::onLocktimeKindChanged,
            qontrol::UNIQUE);
    connect(m_locktime_input, &QLineEdit::textEdited, this, &PathEditor::onLocktimeValueChanged,
            qontrol::UNIQUE);
    for (auto *check : m_key_checks) {
        connect(check, &QCheckBox::toggled, this, &PathEditor::onKeyToggled, qontrol::UNIQUE);
    }
    for (auto *type : m_key_type_combos) {
        connect(type, &QComboBox::activated, this, &PathEditor::onKeyTypeChanged, qontrol::UNIQUE);
    }
    for (auto *mandatory : m_mandatory_checks) {
        connect(mandatory, &QCheckBox::toggled, this, &PathEditor::onMandatoryToggled,
                qontrol::UNIQUE);
    }
    connect(m_remove_btn, &QPushButton::clicked, this, &PathEditor::onRemoveClicked,
            qontrol::UNIQUE);
}

void PathEditor::view() {
    auto *header = (new qontrol::Row)->push(m_title_label)->pushSpacer()->push(m_remove_btn);

    auto *shape = (new qontrol::Row)
                      ->spacing(resolve(Spacing::S))
                      ->push(m_semantic_combo)
                      ->push(m_threshold_combo)
                      ->pushSpacer();
    auto *timelock = (new qontrol::Row)
                         ->spacing(resolve(Spacing::S))
                         ->push(m_locktime_combo)
                         ->push(m_locktime_input)
                         ->pushSpacer();
    auto *fields = (new catalog::FieldGroup)
                       ->addField(TR("musig-create-shape"), shape)
                       ->addField(TR("musig-create-timelock"), timelock);

    auto *keysLabel = new catalog::Label(TR("musig-create-keys"), catalog::LabelRole::Eyebrow);
    keysLabel->setTone(catalog::LabelTone::Muted);

    auto *content = (new qontrol::Column)
                        ->spacing(resolve(Spacing::S))
                        ->push(header)
                        ->push(fields)
                        ->push(keysLabel)
                        ->push(m_keys_column);
    auto *card = new catalog::Card;
    card->setContent(content);
    (new qontrol::Column)->push(card)->into(this);
}

void PathEditor::refreshThresholds() {
    int count = 0;
    for (auto *check : m_key_checks) {
        if (check->isChecked()) {
            ++count;
        }
    }
    int current = m_threshold_combo->currentData().toInt();
    m_threshold_combo->clear();
    for (int m = 1; m <= count; ++m) {
        m_threshold_combo->addItem(TR("musig-create-threshold").arg(m).arg(count), m);
    }
    int threshold = current >= 1 && current <= count ? current : count;
    m_threshold_combo->setCurrentIndex(threshold - 1);
}

void PathEditor::applySemantic() {
    auto current = semantic();
    m_threshold_combo->setVisible(current != PolicySemantic::Single);
    for (auto *mandatory : m_mandatory_checks) {
        mandatory->setVisible(current == PolicySemantic::MultiMandatory);
    }
}

auto PathEditor::semantic() const -> PolicySemantic {
    return static_cast<PolicySemantic>(m_semantic_combo->currentData().toInt());
}

auto PathEditor::locktime() const -> PolicyLocktime {
    return static_cast<PolicyLocktime>(m_locktime_combo->currentData().toInt());
}

// ===== Slots =====

void PathEditor::onSemanticChanged([[maybe_unused]] int index) {
    applySemantic();
    emit edited();
}

void PathEditor::onThresholdChanged([[maybe_unused]] int index) {
    emit edited();
}

void PathEditor::onLocktimeKindChanged([[maybe_unused]] int index) {
    auto kind = locktime();
    bool relative = kind == PolicyLocktime::Relative;
    m_locktime_validator->setTop(relative ? MAX_RELATIVE_BLOCKS : MAX_ABSOLUTE_HEIGHT);
    // A blocks count and a height do not carry over.
    m_locktime_input->clear();
    m_locktime_input->setPlaceholderText(relative ? TR("musig-create-blocks-placeholder")
                                                  : TR("musig-create-height-placeholder"));
    m_locktime_input->setVisible(kind != PolicyLocktime::None);
    emit edited();
}

void PathEditor::onLocktimeValueChanged([[maybe_unused]] const QString &text) {
    emit edited();
}

void PathEditor::onKeyToggled(bool checked) {
    auto index = m_key_checks.indexOf(qobject_cast<catalog::Checkbox *>(sender()));
    if (index >= 0) {
        m_key_type_combos.at(index)->setEnabled(checked);
        m_mandatory_checks.at(index)->setEnabled(checked);
    }
    refreshThresholds();
    emit edited();
}

void PathEditor::onKeyTypeChanged([[maybe_unused]] int index) {
    emit edited();
}

void PathEditor::onMandatoryToggled([[maybe_unused]] bool checked) {
    emit edited();
}

void PathEditor::onRemoveClicked() {
    emit removeRequested(this);
}

} // namespace musig::modal
