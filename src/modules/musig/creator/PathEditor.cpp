#include "modules/musig/creator/PathEditor.h"

#include "catalog/Table.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/DateTimeInput.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QIntValidator>
#include <QSignalBlocker>
#include <Qontrol>
#include <algorithm>
#include <array>
#include <common.h>
#include <utility>

namespace musig::modal {

namespace {

// A count field takes up to six digits: a renewable count from the tip has no other
// bound, and the relative one is checked against its maximum.
constexpr int MAX_COUNT = 999999;
constexpr int CHECK_COLUMN_WIDTH = 40;
constexpr int FINGERPRINT_COLUMN_WIDTH = 110;
constexpr int MANDATORY_COLUMN_WIDTH = 90;
constexpr int COUNT_INPUT_WIDTH = 90;
// A new date defaults to a month away.
constexpr int DEFAULT_DATE_DAYS = 30;

auto lockItems() -> std::array<std::pair<QString, LockKind>, 4> {
    return {{
        {TR("musig-locktime-none"), LockKind::None},
        {TR("musig-locktime-relative"), LockKind::Relative},
        {TR("musig-locktime-renewable"), LockKind::AbsoluteRenewable},
        {TR("musig-locktime-date"), LockKind::Date},
    }};
}

auto unitItems() -> std::array<std::pair<QString, LockUnit>, 4> {
    return {{
        {TR("musig-unit-blocks"), LockUnit::Blocks},
        {TR("musig-unit-days"), LockUnit::Days},
        {TR("musig-unit-weeks"), LockUnit::Weeks},
        {TR("musig-unit-months"), LockUnit::Months},
    }};
}

void selectData(catalog::ComboBox *combo, int data) {
    int index = combo->findData(data);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}

} // namespace

PathEditor::PathEditor(const QList<KeyEntry> &keys, QWidget *parent)
    : QWidget(parent),
      m_keys(keys) {
    init();
    doConnect();
    view();
    refresh();
}

void PathEditor::setPath(const SpendPath &path) {
    m_path = path;
    // A key that cannot sign MuSig2 is never picked.
    for (const auto &entry : m_keys) {
        if (!entry.can_musig) {
            m_path.keys.removeAll(entry.fingerprint);
            m_path.mandatory.removeAll(entry.fingerprint);
        }
    }
    QSignalBlocker nameBlocker(m_name_input);
    m_name_input->setText(m_path.name);
    for (int i = 0; i < m_keys.size(); ++i) {
        const auto &fingerprint = m_keys.at(i).fingerprint;
        QSignalBlocker checkBlocker(m_key_checks.at(i));
        QSignalBlocker mandatoryBlocker(m_mandatory_checks.at(i));
        m_key_checks.at(i)->setChecked(m_keys.at(i).can_musig && m_path.keys.contains(fingerprint));
        m_mandatory_checks.at(i)->setChecked(m_path.mandatory.contains(fingerprint));
    }
    QSignalBlocker lockBlocker(m_lock_combo);
    QSignalBlocker countBlocker(m_count_input);
    QSignalBlocker unitBlocker(m_unit_combo);
    QSignalBlocker dateBlocker(m_date_input);
    selectData(m_lock_combo, static_cast<int>(m_path.lock));
    m_count_input->setText(m_path.count > 0 ? QString::number(m_path.count) : QString());
    selectData(m_unit_combo, static_cast<int>(m_path.unit));
    m_date_input->setDateTime(m_path.date.isValid()
                                  ? m_path.date
                                  : QDateTime::currentDateTime().addDays(DEFAULT_DATE_DAYS));
    refresh();
}

auto PathEditor::path() const -> SpendPath {
    return m_path;
}

void PathEditor::setTip(std::optional<std::uint32_t> tip) {
    m_tip = tip;
    refresh();
}

void PathEditor::init() {
    m_name_input = new catalog::Input;
    m_name_input->setWidth(Size::L);
    m_name_input->setPlaceholderText(TR("musig-path-name-placeholder"));

    if (m_keys.isEmpty()) {
        m_keys_table = new catalog::EmptyState(icon::key, TR("create-account-no-key-title"),
                                               TR("create-account-no-key-sub"));
    } else {
        auto *table = new catalog::Table;
        table->addColumn({.width = CHECK_COLUMN_WIDTH});
        table->addColumn({.title = TR("musig-path-key"), .grow = true});
        table->addColumn(
            {.title = TR("musig-path-fingerprint"), .width = FINGERPRINT_COLUMN_WIDTH});
        table->addColumn({.title = TR("musig-path-mandatory"), .width = MANDATORY_COLUMN_WIDTH});
        for (const auto &entry : m_keys) {
            auto *check = new catalog::Checkbox;
            auto *mandatory = new catalog::Checkbox;
            auto *name = new catalog::Label(entry.alias.isEmpty() ? entry.fingerprint : entry.alias,
                                            catalog::LabelRole::Body);
            QWidget *keyCell = name;
            if (!entry.can_musig) {
                // Shown, not hidden: the user sees why the key cannot be picked.
                check->setEnabled(false);
                name->setTone(catalog::LabelTone::Muted);
                auto *reason =
                    new catalog::Label(TR("musig-path-key-no-musig"), catalog::LabelRole::Caption);
                reason->setTone(catalog::LabelTone::Muted);
                keyCell = (new qontrol::Column)->spacing(0)->push(name)->push(reason);
            }
            auto *fingerprint = new catalog::Label(entry.fingerprint, catalog::LabelRole::Mono);
            fingerprint->setTone(catalog::LabelTone::Muted);
            m_key_checks.append(check);
            m_mandatory_checks.append(mandatory);
            table->addWidgetRow(entry.fingerprint, {check, keyCell, fingerprint, mandatory});
        }
        m_keys_table = table;
    }

    m_threshold_combo = new catalog::ComboBox;
    m_threshold_combo->setWidth(Size::M);

    m_lock_combo = new catalog::ComboBox;
    m_lock_combo->setWidth(Size::M);
    for (const auto &[label, kind] : lockItems()) {
        m_lock_combo->addItem(label, static_cast<int>(kind));
    }
    m_count_input = new catalog::Input(catalog::InputRole::Mono);
    m_count_input->setFixedWidth(COUNT_INPUT_WIDTH);
    m_count_input->setPlaceholderText(QStringLiteral("0"));
    m_count_validator = new QIntValidator(1, MAX_COUNT, m_count_input);
    m_count_input->setValidator(m_count_validator);
    m_unit_combo = new catalog::ComboBox;
    m_unit_combo->setWidth(Size::S);
    for (const auto &[label, unit] : unitItems()) {
        m_unit_combo->addItem(label, static_cast<int>(unit));
    }
    m_date_input = new catalog::DateTimeInput;
    m_date_input->setWidth(Size::M);
    m_hint_label = new catalog::Label(catalog::LabelRole::Caption);
    m_hint_label->setTone(catalog::LabelTone::Muted);
}

void PathEditor::doConnect() {
    connect(m_name_input, &QLineEdit::textEdited, this, &PathEditor::onNameEdited, qontrol::UNIQUE);
    for (auto *check : m_key_checks) {
        connect(check, &QCheckBox::toggled, this, &PathEditor::onKeyToggled, qontrol::UNIQUE);
    }
    for (auto *mandatory : m_mandatory_checks) {
        connect(mandatory, &QCheckBox::toggled, this, &PathEditor::onMandatoryToggled,
                qontrol::UNIQUE);
    }
    connect(m_threshold_combo, &QComboBox::activated, this, &PathEditor::onThresholdChanged,
            qontrol::UNIQUE);
    connect(m_lock_combo, &QComboBox::activated, this, &PathEditor::onLockKindChanged,
            qontrol::UNIQUE);
    connect(m_count_input, &QLineEdit::textEdited, this, &PathEditor::onCountEdited,
            qontrol::UNIQUE);
    connect(m_unit_combo, &QComboBox::activated, this, &PathEditor::onUnitChanged, qontrol::UNIQUE);
    connect(m_date_input, &QDateTimeEdit::dateTimeChanged, this, &PathEditor::onDateChanged,
            qontrol::UNIQUE);
}

void PathEditor::view() {
    auto *name = (new catalog::FieldGroup)->addField(TR("musig-path-name"), m_name_input);

    auto *keysLabel = new catalog::Label(TR("musig-create-keys"), catalog::LabelRole::Eyebrow);
    keysLabel->setTone(catalog::LabelTone::Muted);

    auto *timelock = (new qontrol::Row)
                         ->spacing(resolve(Spacing::XS))
                         ->push(m_lock_combo)
                         ->push(m_count_input)
                         ->push(m_unit_combo)
                         ->push(m_date_input)
                         ->push(m_hint_label)
                         ->pushSpacer();
    m_fields = (new catalog::FieldGroup)
                   ->addField(TR("musig-path-signatures"), m_threshold_combo)
                   ->addField(TR("musig-create-timelock"), timelock);

    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(name)
        ->push((new qontrol::Column)
                   ->spacing(resolve(Spacing::XS))
                   ->push(keysLabel)
                   ->push(m_keys_table))
        ->push(m_fields)
        ->into(this);
}

void PathEditor::readPath() {
    m_path.name = m_name_input->text();
    m_path.keys.clear();
    m_path.mandatory.clear();
    for (int i = 0; i < m_keys.size(); ++i) {
        if (!m_key_checks.at(i)->isChecked()) {
            continue;
        }
        m_path.keys.append(m_keys.at(i).fingerprint);
        // A lone key cannot be mandatory: it signs every spend anyway.
        if (m_mandatory_checks.at(i)->isChecked()) {
            m_path.mandatory.append(m_keys.at(i).fingerprint);
        }
    }
    if (m_path.keys.size() < 2) {
        m_path.mandatory.clear();
    }
    auto picked = static_cast<int>(m_path.keys.size());
    auto mandatory = static_cast<int>(m_path.mandatory.size());
    m_path.threshold = std::clamp(m_path.threshold, std::min(mandatory + 1, std::max(picked, 1)),
                                  std::max(picked, 1));
    m_path.lock = static_cast<LockKind>(m_lock_combo->currentData().toInt());
    m_path.count = m_count_input->text().toInt();
    m_path.unit = static_cast<LockUnit>(m_unit_combo->currentData().toInt());
    m_path.date = m_path.lock == LockKind::Date ? m_date_input->dateTime() : QDateTime();
}

void PathEditor::refresh() {
    auto picked = static_cast<int>(m_path.keys.size());
    auto mandatory = static_cast<int>(m_path.mandatory.size());
    for (int i = 0; i < m_keys.size(); ++i) {
        bool checked = m_key_checks.at(i)->isChecked();
        auto *mandatoryCheck = m_mandatory_checks.at(i);
        mandatoryCheck->setVisible(checked && picked > 1);
        // At least one key stays optional.
        mandatoryCheck->setEnabled(mandatoryCheck->isChecked() || mandatory + 1 < picked);
    }
    refreshThresholds();
    m_fields->setFieldVisible(m_threshold_combo, picked > 1);

    bool counted = m_path.lock == LockKind::Relative || m_path.lock == LockKind::AbsoluteRenewable;
    m_count_input->setVisible(counted);
    m_unit_combo->setVisible(counted);
    m_date_input->setVisible(m_path.lock == LockKind::Date);
    m_date_input->setMinimumDateTime(QDateTime::currentDateTime());
    auto hint = lockHint(m_path, m_tip);
    m_hint_label->setText(hint);
    m_hint_label->setVisible(!hint.isEmpty());
}

void PathEditor::refreshThresholds() {
    QSignalBlocker blocker(m_threshold_combo);
    m_threshold_combo->clear();
    auto picked = static_cast<int>(m_path.keys.size());
    auto lowest = std::min(static_cast<int>(m_path.mandatory.size()) + 1, std::max(picked, 1));
    for (int threshold = lowest; threshold <= picked; ++threshold) {
        auto label = threshold == picked ? TR("musig-path-all").arg(picked)
                                         : TR("musig-create-threshold").arg(threshold).arg(picked);
        m_threshold_combo->addItem(label, threshold);
    }
    selectData(m_threshold_combo, m_path.threshold);
}

// ===== Slots =====

void PathEditor::onNameEdited([[maybe_unused]] const QString &text) {
    readPath();
    emit edited();
}

void PathEditor::onKeyToggled(bool checked) {
    auto index = m_key_checks.indexOf(qobject_cast<catalog::Checkbox *>(sender()));
    // An unpicked key is no longer mandatory either.
    if (!checked && index >= 0) {
        QSignalBlocker blocker(m_mandatory_checks.at(index));
        m_mandatory_checks.at(index)->setChecked(false);
    }
    readPath();
    refresh();
    emit edited();
}

void PathEditor::onMandatoryToggled([[maybe_unused]] bool checked) {
    readPath();
    refresh();
    emit edited();
}

void PathEditor::onThresholdChanged([[maybe_unused]] int index) {
    m_path.threshold = m_threshold_combo->currentData().toInt();
    emit edited();
}

void PathEditor::onLockKindChanged([[maybe_unused]] int index) {
    // A count or a date does not carry over to another kind.
    m_count_input->clear();
    readPath();
    refresh();
    emit edited();
}

void PathEditor::onCountEdited([[maybe_unused]] const QString &text) {
    readPath();
    refresh();
    emit edited();
}

void PathEditor::onUnitChanged([[maybe_unused]] int index) {
    readPath();
    refresh();
    emit edited();
}

void PathEditor::onDateChanged([[maybe_unused]] const QDateTime &date) {
    readPath();
    refresh();
    emit edited();
}

} // namespace musig::modal
