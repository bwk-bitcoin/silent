#include "HotKeyFlow.h"

#include "KeyChecks.h"
#include "KeyFields.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/display/WordGrid.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/form/WordInputGrid.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

const char *const K_HOT_KIND = "hotKind";

constexpr int MIN_PASSWORD_LENGTH = 8;
constexpr int WORDS_SHORT = 12;
constexpr int WORDS_MIDDLE = 15;
constexpr int WORDS_LONG = 24;

auto hotRow(int kind, IconFn icon, const QString &title, const QString &sub)
    -> catalog::SelectRow * {
    auto *row = new catalog::SelectRow;
    row->setIcon(icon);
    row->setTitle(title);
    row->setSub(sub);
    row->setProperty(K_HOT_KIND, kind);
    return row;
}

} // namespace

HotKeyFlow::HotKeyFlow(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

HotKeyFlow::~HotKeyFlow() {
    clearSecrets();
}

void HotKeyFlow::start() {
    showHotChoose();
}

void HotKeyFlow::init() {
    initHotChoose();
    initBackup();
    initHotPassword();
    initStatus();
}

void HotKeyFlow::doConnect() {
    connectHotChoose();
    connectBackup();
    connectHotPassword();
    connectStatus();
}

void HotKeyFlow::view() {
    m_choose_body = viewHotChoose();
    m_backup_body = viewBackup();
    m_password_body = viewHotPassword();
    hideBackup();
    hideHotPassword();
    hideStatus();
}

// ===== Hot key choice view =====

void HotKeyFlow::initHotChoose() {
    m_generate12 = hotRow(static_cast<int>(HotKind::Generate12), icon::refresh,
                          TR("keys-hot-generate-12"), TR("keys-hot-generate-12-sub"));
    m_generate24 = hotRow(static_cast<int>(HotKind::Generate24), icon::refresh,
                          TR("keys-hot-generate-24"), TR("keys-hot-generate-24-sub"));
    m_import = hotRow(static_cast<int>(HotKind::Import), icon::pencil, TR("keys-hot-import-words"),
                      TR("keys-hot-import-sub"));
    m_word_count = new catalog::ComboBox;
    for (int count : {WORDS_SHORT, WORDS_MIDDLE, WORDS_LONG}) {
        m_word_count->addItem(TR("keys-words-count").arg(count), count);
    }
    m_word_count->setWidth(Size::S);
    m_import->setTrailing(m_word_count);
    m_words_input = new catalog::WordInputGrid;
    m_words_input->setWordCount(WORDS_SHORT);
    m_words_input->setWordCheck(
        [](const QString &word) { return ::bip39_word_is_valid(word.toStdString()); });
    m_choose_continue = catalog::btnContinue();
    m_choose_actions = new catalog::ModalActions;
    m_choose_actions->setBack(catalog::ModalActions::Back::Back);
    m_choose_actions->addAction(m_choose_continue);
}

void HotKeyFlow::connectHotChoose() {
    for (auto *row : {m_generate12, m_generate24, m_import}) {
        connect(row, &catalog::SelectRow::clicked, this, &HotKeyFlow::onHotKindClicked,
                qontrol::UNIQUE);
    }
    connect(m_word_count, &QComboBox::activated, this, &HotKeyFlow::onWordCountPicked,
            qontrol::UNIQUE);
    connect(m_words_input, &catalog::WordInputGrid::wordsChanged, this,
            &HotKeyFlow::onHotChooseChanged, qontrol::UNIQUE);
    connect(m_choose_continue, &QPushButton::clicked, this, &HotKeyFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_choose_actions, &catalog::ModalActions::backClicked, this, &HotKeyFlow::onBackClicked,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::mnemonicGenerated, this, &HotKeyFlow::onMnemonicGenerated,
            qontrol::UNIQUE);
}

auto HotKeyFlow::viewHotChoose() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push((new qontrol::Column)
                   ->spacing(resolve(Spacing::S))
                   ->push(m_generate12)
                   ->push(m_generate24)
                   ->push(m_import))
        ->push(m_words_input);
}

void HotKeyFlow::showHotChoose() {
    m_view = View::HotChoose;
    onHotChooseChanged();
    viewSlots()->setHeader(nullptr);
    viewSlots()->setWideBody(m_choose_body);
    viewSlots()->setFooter(m_choose_actions);
    emit viewChanged(TR("keys-add-hot-title"));
}

auto HotKeyFlow::typedMnemonic() const -> QString {
    return m_words_input->words().join(QLatin1Char(' ')).trimmed().toLower();
}

void HotKeyFlow::continueHot() {
    switch (m_hot_kind) {
    case HotKind::Import:
        m_mnemonic = typedMnemonic();
        showHotPassword();
        return;
    case HotKind::Generate12:
    case HotKind::Generate24:
        m_choose_continue->setEnabled(false);
        m_generate_req = m_module->nextReqId();
        m_module->generateMnemonic(m_generate_req,
                                   m_hot_kind == HotKind::Generate12 ? WORDS_SHORT : WORDS_LONG);
        return;
    }
}

// ===== Backup view =====

void HotKeyFlow::initBackup() {
    m_backup_words = new catalog::WordGrid;
    m_backup_check = new catalog::Checkbox(TR("keys-backup-wrote"));
    m_backup_continue = catalog::btnContinue();
    m_backup_actions = new catalog::ModalActions;
    m_backup_actions->setBack(catalog::ModalActions::Back::Back);
    m_backup_actions->addAction(m_backup_continue);
}

void HotKeyFlow::connectBackup() {
    connect(m_backup_check, &QCheckBox::toggled, this, &HotKeyFlow::onBackupChanged,
            qontrol::UNIQUE);
    connect(m_backup_continue, &QPushButton::clicked, this, &HotKeyFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_backup_actions, &catalog::ModalActions::backClicked, this, &HotKeyFlow::onBackClicked,
            qontrol::UNIQUE);
}

auto HotKeyFlow::viewBackup() -> QWidget * {
    auto *warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    warning->setTitle(TR("keys-backup-title"));
    warning->setText(TR("keys-backup-body"));
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(warning)
        ->push(m_backup_words)
        ->push(m_backup_check);
}

void HotKeyFlow::showBackup() {
    m_view = View::Backup;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setWideBody(m_backup_body);
    viewSlots()->setFooter(m_backup_actions);
    emit viewChanged(TR("keys-add-backup-title"));
}

void HotKeyFlow::hideBackup() {
    viewSlots()->keep(m_backup_body);
    viewSlots()->keep(m_backup_actions);
}

// ===== Hot key password view =====

void HotKeyFlow::initHotPassword() {
    m_hot_alias = aliasInput();
    m_password = new catalog::Input;
    m_password->setEchoMode(QLineEdit::Password);
    m_password_short =
        new catalog::FieldNote(TR("keys-password-short"), catalog::FieldNote::Tone::Error);
    m_confirm = new catalog::Input;
    m_confirm->setEchoMode(QLineEdit::Password);
    m_password_mismatch =
        new catalog::FieldNote(TR("keys-password-no-match"), catalog::FieldNote::Tone::Error);
    m_seal = catalog::btnSealKey();
    m_password_actions = new catalog::ModalActions;
    m_password_actions->setBack(catalog::ModalActions::Back::Back);
    m_password_actions->addAction(m_seal);
}

void HotKeyFlow::connectHotPassword() {
    for (auto *input : {m_hot_alias, m_password, m_confirm}) {
        connect(input, &QLineEdit::textEdited, this, &HotKeyFlow::onHotPasswordChanged,
                qontrol::UNIQUE);
    }
    connect(m_seal, &QPushButton::clicked, this, &HotKeyFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_password_actions, &catalog::ModalActions::backClicked, this,
            &HotKeyFlow::onBackClicked, qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotKeyImported, this, &HotKeyFlow::onHotKeyImported,
            qontrol::UNIQUE);
}

auto HotKeyFlow::viewHotPassword() -> QWidget * {
    auto *lead = new Label(TR("keys-password-lead"), LabelRole::Body);
    lead->setTone(LabelTone::Secondary);
    lead->setWordWrap(true);
    m_password_fields = new catalog::FieldGroup;
    m_password_fields->addField(TR("keys-alias"), m_hot_alias)
        ->addField(TR("keys-password-field"), m_password)
        ->addNote(m_password_short)
        ->addField(TR("keys-password-confirm-field"), m_confirm)
        ->addNote(m_password_mismatch);
    return (new qontrol::Column)->spacing(resolve(Spacing::S))->push(lead)->push(m_password_fields);
}

void HotKeyFlow::showHotPassword() {
    m_view = View::HotPassword;
    onHotPasswordChanged();
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_password_body);
    viewSlots()->setFooter(m_password_actions);
    emit viewChanged(TR("keys-add-password-title"));
}

void HotKeyFlow::hideHotPassword() {
    viewSlots()->keep(m_password_body);
    viewSlots()->keep(m_password_actions);
}

void HotKeyFlow::sealKey() {
    m_status->setSpinner();
    m_status->setTitle(TR("keys-sealing-title"));
    m_status->setSubtitle(TR("keys-sealing-sub"));
    showSealing();
    m_seal_req = m_module->nextReqId();
    m_module->importHotKey(m_seal_req, m_mnemonic, m_hot_alias->text().trimmed(),
                           m_password->text());
}

// ===== Status view =====

void HotKeyFlow::initStatus() {
    m_status = new catalog::ModalStatus;
    m_status->setPadding(resolve(Spacing::M), resolve(Spacing::M));
    m_done = catalog::btnDone();
    m_try_again = catalog::btnTryAgain();
    m_status_actions = new catalog::ModalActions;
    m_status_actions->addAction(m_done);
    m_status_actions->addAction(m_try_again);
}

void HotKeyFlow::connectStatus() {
    connect(m_done, &QPushButton::clicked, this, &HotKeyFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_try_again, &QPushButton::clicked, this, &HotKeyFlow::onFailureRetry, qontrol::UNIQUE);
}

void HotKeyFlow::showSealing() {
    m_view = View::Sealing;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_status);
    viewSlots()->setFooter(nullptr);
    emit viewChanged(TR("keys-add-sealing-title"));
}

void HotKeyFlow::showResult() {
    m_view = View::Result;
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_status);
    viewSlots()->setFooter(m_status_actions);
    emit viewChanged(TR("keys-add-title"));
}

void HotKeyFlow::hideStatus() {
    viewSlots()->keep(m_status);
    viewSlots()->keep(m_status_actions);
}

void HotKeyFlow::showFailure(const QString &title, const QString &message) {
    m_status->setIcon(icon::circleX, IconColor::Error);
    m_status->setTitle(title);
    m_status->setSubtitle(message);
    m_done->setVisible(false);
    m_try_again->setVisible(true);
    showResult();
}

// ===== Shared =====

void HotKeyFlow::clearSecrets() {
    // A QString cannot be zeroized: fill() and clear() are best effort. Rust owns the
    // real wiping of the mnemonic and the password it was handed.
    m_mnemonic.fill(QChar());
    m_mnemonic.clear();
    m_backup_words->setWords({});
    m_password->clear();
    m_confirm->clear();
}

// ===== Slots =====

void HotKeyFlow::onBackClicked() {
    switch (m_view) {
    case View::HotChoose:
        emit back();
        return;
    case View::Backup:
        showHotChoose();
        return;
    case View::HotPassword:
        if (m_hot_kind == HotKind::Import) {
            showHotChoose();
            return;
        }
        showBackup();
        return;
    case View::Sealing:
    case View::Result:
        return;
    }
}

void HotKeyFlow::onNextClicked() {
    switch (m_view) {
    case View::HotChoose:
        continueHot();
        return;
    case View::Backup:
        showHotPassword();
        return;
    case View::HotPassword:
        sealKey();
        return;
    case View::Result:
        emit added();
        return;
    case View::Sealing:
        return;
    }
}

void HotKeyFlow::onHotKindClicked() {
    m_hot_kind = static_cast<HotKind>(sender()->property(K_HOT_KIND).toInt());
    showHotChoose();
}

void HotKeyFlow::onWordCountPicked() {
    m_hot_kind = HotKind::Import;
    m_words_input->setWordCount(m_word_count->currentData().toInt());
    onHotChooseChanged();
}

void HotKeyFlow::onHotChooseChanged() {
    m_generate12->setSelected(m_hot_kind == HotKind::Generate12);
    m_generate24->setSelected(m_hot_kind == HotKind::Generate24);
    m_import->setSelected(m_hot_kind == HotKind::Import);
    bool importing = m_hot_kind == HotKind::Import;
    m_words_input->setVisible(importing);

    QString reason;
    if (importing) {
        int missing = 0;
        int unknown = 0;
        for (const auto &word : m_words_input->words()) {
            if (word.isEmpty()) {
                ++missing;
            } else if (!::bip39_word_is_valid(word.toStdString())) {
                ++unknown;
            }
        }
        if (unknown > 0) {
            reason = unknown == 1 ? TR("keys-words-unknown-one")
                                  : TR("keys-words-unknown-many").arg(unknown);
        } else if (missing > 0) {
            reason = missing == 1 ? TR("keys-words-missing-one")
                                  : TR("keys-words-missing-many").arg(missing);
        } else if (!::mnemonic_is_valid(typedMnemonic().toStdString())) {
            reason = TR("keys-words-checksum");
        }
    }
    m_choose_continue->setEnabled(reason.isEmpty() && m_generate_req == 0);
    m_choose_actions->setReason(reason);
    window()->adjustSize();
}

void HotKeyFlow::onMnemonicGenerated(ReqId req_id, const MnemonicResult &result) {
    if (m_generate_req == 0 || req_id != m_generate_req) {
        return;
    }
    m_generate_req = 0;
    if (!result.is_ok) {
        onHotChooseChanged();
        showFailure(TR("keys-generate-failed-title"), KeysModule::toQt(result.error));
        return;
    }
    m_mnemonic = KeysModule::toQt(result.mnemonic);
    m_backup_words->setWords(m_mnemonic.split(QLatin1Char(' '), Qt::SkipEmptyParts));
    m_backup_check->setChecked(false);
    onBackupChanged();
    showBackup();
}

void HotKeyFlow::onBackupChanged() {
    bool wrote = m_backup_check->isChecked();
    m_backup_continue->setEnabled(wrote);
    m_backup_actions->setReason(wrote ? QString() : TR("keys-backup-reason"));
}

void HotKeyFlow::onHotPasswordChanged() {
    QString password = m_password->text();
    bool tooShort = !password.isEmpty() && password.size() < MIN_PASSWORD_LENGTH;
    bool mismatch = !m_confirm->text().isEmpty() && m_confirm->text() != password;
    m_password_short->setVisible(tooShort);
    m_password_mismatch->setVisible(mismatch);

    QString reason = aliasReason(m_module, m_hot_alias->text().trimmed());
    if (reason.isEmpty() && password.isEmpty()) {
        reason = TR("keys-password-choose");
    } else if (reason.isEmpty() && tooShort) {
        reason = TR("keys-password-short");
    } else if (reason.isEmpty() && m_confirm->text() != password) {
        reason = TR("keys-password-must-match");
    }
    m_seal->setEnabled(reason.isEmpty());
    m_password_actions->setReason(reason);
    window()->adjustSize();
}

void HotKeyFlow::onHotKeyImported(ReqId req_id, const KeyInfoResult &result) {
    if (m_seal_req == 0 || req_id != m_seal_req) {
        return;
    }
    m_seal_req = 0;
    if (!result.is_ok) {
        showFailure(TR("keys-seal-failed-title"), KeysModule::toQt(result.error));
        return;
    }
    clearSecrets();
    m_status->setIcon(icon::circleCheck, IconColor::Success);
    m_status->setTitle(TR("keys-added-title"));
    m_status->setSubtitle(
        TR("keys-added-sub")
            .arg(KeysModule::toQt(result.key.alias), KeysModule::toQt(result.key.fingerprint)));
    m_done->setVisible(true);
    m_try_again->setVisible(false);
    showResult();
}

void HotKeyFlow::onFailureRetry() {
    if (m_mnemonic.isEmpty()) {
        showHotChoose();
        return;
    }
    sealKey();
}

} // namespace keys::modal
