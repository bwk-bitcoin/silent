#include "HotKey.h"

#include "PasswordPrompt.h"

#include "modules/keys/KeysModule.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/form/FormRow.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/TextEdit.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <common.h>

using catalog::Button;
using catalog::ButtonRole;
using catalog::Checkbox;
using catalog::ComboBox;
using catalog::FormRow;
using catalog::Input;
using catalog::Label;
using catalog::LabelRole;
using catalog::ModalStatus;
using catalog::TextEdit;

namespace keys::modal {

namespace {

const char *const K_HOT_KEY_MODE = "hotKeyMode";

constexpr std::uint32_t WORD_COUNT_12 = 12;
constexpr std::uint32_t WORD_COUNT_24 = 24;

bool isValidWordCount(std::uint32_t count) {
    return count == WORD_COUNT_12 || count == WORD_COUNT_24;
}

} // namespace

HotKey::HotKey(KeysModule *module, QWidget *parent) : m_module(module) {
    Q_UNUSED(parent);
    setWindowTitle(TR("keys-hot-title"));
    init();
    doConnect();
    view();
    applyStep();
}

HotKey::~HotKey() {
    clearSecrets();
}

void HotKey::init() {
    m_generate_tab = new catalog::SelectRow;
    m_generate_tab->setIcon(icon::key);
    m_generate_tab->setTitle(TR("keys-hot-generate"));
    m_generate_tab->setProperty(K_HOT_KEY_MODE, static_cast<int>(Mode::Generate));

    m_import_tab = new catalog::SelectRow;
    m_import_tab->setIcon(icon::lock);
    m_import_tab->setTitle(TR("keys-hot-import"));
    m_import_tab->setProperty(K_HOT_KEY_MODE, static_cast<int>(Mode::Import));

    m_alias_input = new Input;
    m_alias_input->setPlaceholderText(TR("keys-alias-placeholder"));

    m_word_count = new ComboBox;
    m_word_count->addItem(TR("keys-words-12"), WORD_COUNT_12);
    m_word_count->addItem(TR("keys-words-24"), WORD_COUNT_24);

    m_mnemonic_input = new TextEdit;
    m_mnemonic_input->setPlaceholderText(TR("keys-mnemonic-placeholder"));
    m_mnemonic_input->setMinimumWidth(260);

    m_mnemonic_label = new Label(QString(), LabelRole::Mono);
    m_mnemonic_label->setWordWrap(true);

    m_backup_check = new Checkbox(TR("keys-backup-confirm"));

    auto *backupWarning = new Label(TR("keys-backup-warning"), LabelRole::Caption);
    backupWarning->setWordWrap(true);

    m_error_label = new Label(QString(), LabelRole::Caption);
    m_error_label->setVisible(false);

    m_status = new ModalStatus;

    m_generate_btn = new Button(TR("common-generate"), ButtonRole::Primary);
    m_continue_btn = new Button(TR("common-proceed"), ButtonRole::Primary);
    m_continue_btn->setEnabled(false);
    m_cancel_btn = new Button(TR("common-cancel"));

    m_generate_row = m_word_count;
    m_import_row = m_mnemonic_input;
    m_backup_row = (new qontrol::Column)
                       ->push(m_mnemonic_label)
                       ->pushSpacer(resolve(Spacing::S))
                       ->push(backupWarning)
                       ->pushSpacer(resolve(Spacing::S))
                       ->push(m_backup_check);
}

void HotKey::doConnect() {
    connect(m_generate_tab, &catalog::SelectRow::clicked, this, &HotKey::onModeClicked,
            qontrol::UNIQUE);
    connect(m_import_tab, &catalog::SelectRow::clicked, this, &HotKey::onModeClicked,
            qontrol::UNIQUE);
    connect(m_generate_btn, &QPushButton::clicked, this, &HotKey::onGenerateClicked,
            qontrol::UNIQUE);
    connect(m_backup_check, &QCheckBox::toggled, this, &HotKey::onBackupToggled, qontrol::UNIQUE);
    connect(m_continue_btn, &QPushButton::clicked, this, &HotKey::onContinueClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &QDialog::reject, qontrol::UNIQUE);
    connect(m_module, &KeysModule::mnemonicGenerated, this, &HotKey::onMnemonicGenerated,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotKeyImported, this, &HotKey::onHotKeyImported,
            qontrol::UNIQUE);
}

void HotKey::view() {
    auto *tabRow = (new qontrol::Row)
                       ->push(m_generate_tab)
                       ->pushSpacer(resolve(Spacing::XS))
                       ->push(m_import_tab);

    auto *buttonRow = (new qontrol::Row)
                          ->pushSpacer()
                          ->push(m_cancel_btn)
                          ->pushSpacer(resolve(Spacing::XS))
                          ->push(m_generate_btn)
                          ->pushSpacer(resolve(Spacing::XS))
                          ->push(m_continue_btn);

    auto *col = (new qontrol::Column)
                    ->push(new Label(TR("keys-hot-title"), LabelRole::Heading))
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(tabRow)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(new FormRow(TR("keys-alias"), m_alias_input))
                    ->pushSpacer(resolve(Spacing::XS))
                    ->push(m_generate_row)
                    ->push(m_import_row)
                    ->push(m_backup_row)
                    ->push(m_status)
                    ->push(m_error_label)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(buttonRow);

    setMainWidget(margin(col));
}

void HotKey::applyStep() {
    bool pickGenerate = (m_step == Step::Pick && m_mode == Mode::Generate);
    bool pickImport = (m_step == Step::Pick && m_mode == Mode::Import);
    bool backup = (m_step == Step::Backup);
    bool working = (m_step == Step::Working);

    m_generate_row->setVisible(pickGenerate);
    m_import_row->setVisible(pickImport);
    m_backup_row->setVisible(backup);
    m_status->setVisible(working);

    m_generate_btn->setEnabled(pickGenerate);
    m_continue_btn->setEnabled(pickImport || (backup && m_backup_check->isChecked()));
    m_cancel_btn->setEnabled(!working);
}

void HotKey::showError(const QString &message) {
    m_error_label->setText(message);
    m_error_label->setVisible(!message.isEmpty());
}

void HotKey::clearSecrets() {
    // A QString cannot be zeroized: fill()+clear() is best effort. Rust owns the
    // real wiping of the mnemonic and password it was handed.
    m_mnemonic.fill(QChar());
    m_mnemonic.clear();
    m_mnemonic_label->setText(QString());
    m_mnemonic_input->setText(QString());
}

void HotKey::onModeClicked() {
    if (m_step != Step::Pick) {
        return;
    }
    auto *row = qobject_cast<QObject *>(sender());
    if (row == nullptr) {
        return;
    }
    m_mode = static_cast<Mode>(row->property(K_HOT_KEY_MODE).toInt());
    applyStep();
}

void HotKey::onGenerateClicked() {
    auto count = static_cast<std::uint32_t>(m_word_count->currentData().toInt());
    if (!isValidWordCount(count)) {
        showError(TR("keys-mnemonic-bad-length"));
        return;
    }

    m_generate_btn->setEnabled(false);
    m_mnemonic_req_id = m_module->nextReqId();
    m_module->generateMnemonic(m_mnemonic_req_id, count);
}

void HotKey::onBackupToggled(bool checked) {
    if (m_step != Step::Backup) {
        return;
    }
    m_continue_btn->setEnabled(checked);
}

void HotKey::onContinueClicked() {
    if (m_step == Step::Pick && m_mode == Mode::Import) {
        QString mnemonic = m_mnemonic_input->toPlainText().simplified().toLower();
        auto wordCount = static_cast<std::uint32_t>(
            mnemonic.isEmpty() ? 0 : mnemonic.count(QLatin1Char(' ')) + 1);
        if (!isValidWordCount(wordCount)) {
            showError(TR("keys-mnemonic-bad-length"));
            return;
        }
        m_mnemonic = mnemonic;
    } else if (m_step != Step::Backup) {
        return;
    }

    auto *prompt = new PasswordPrompt(PasswordPrompt::Mode::Create, TR("keys-password-create"));
    connect(prompt, &PasswordPrompt::passwordEntered, this, &HotKey::onPasswordEntered,
            qontrol::UNIQUE);
    AppController::execModal(prompt);
}

void HotKey::onPasswordEntered(QString password) {
    m_step = Step::Working;
    applyStep();
    m_status->setState(ModalStatus::State::Signing);
    m_status->setSubtitle(TR("keys-hot-working"));

    QString alias = m_alias_input->text().trimmed();
    m_import_req_id = m_module->nextReqId();
    m_module->importHotKey(m_import_req_id, m_mnemonic, alias, password);

    password.fill(QChar());
    password.clear();
}

void HotKey::onMnemonicGenerated(ReqId req_id, MnemonicResult result) {
    if (req_id != m_mnemonic_req_id) {
        return;
    }
    m_generate_btn->setEnabled(true);
    if (!result.is_ok) {
        showError(TR("keys-hot-failed").arg(KeysModule::toQt(result.error)));
        return;
    }

    m_mnemonic = KeysModule::toQt(result.mnemonic);
    m_mnemonic_label->setText(m_mnemonic);
    m_step = Step::Backup;
    applyStep();
}

void HotKey::onHotKeyImported(ReqId req_id, KeyInfoResult result) {
    if (req_id != m_import_req_id) {
        return;
    }
    if (!result.is_ok) {
        m_step = (m_mode == Mode::Generate) ? Step::Backup : Step::Pick;
        applyStep();
        showError(TR("keys-hot-failed").arg(KeysModule::toQt(result.error)));
        return;
    }

    QString fingerprint = KeysModule::toQt(result.key.fingerprint);
    clearSecrets();
    emit keyAdded(fingerprint);
    accept();
}

} // namespace keys::modal
