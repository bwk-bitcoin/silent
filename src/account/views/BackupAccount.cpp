#include "account/views/BackupAccount.h"

#include "account/RequestId.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "theme/Palette.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <Qontrol>
#include <common.h>

namespace account::view {

namespace {

const char *const BACKUP_SUFFIX = ".silent-backup";
constexpr int MIN_PASSWORD_LENGTH = 8;

} // namespace

auto writeBackupFile(const QString &path, const QByteArray &backup) -> QString {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(backup) != backup.size()) {
        return file.errorString();
    }
    return {};
}

auto backupFileName(const QString &name) -> QString {
    static const QRegularExpression c_unsafe(QStringLiteral("[^A-Za-z0-9._-]+"));
    auto base = QString(name).replace(c_unsafe, QStringLiteral("_"));
    if (base.isEmpty()) {
        base = QStringLiteral("account");
    }
    return base + QStringLiteral("-") +
           QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")) + BACKUP_SUFFIX;
}

BackupAccount::BackupAccount(backend::IAccountBackend *account) : m_account(account) {
    init();
    doConnect();
    view();
}

BackupAccount::~BackupAccount() {
    clearPassword();
}

void BackupAccount::init() {
    initPassword();
    initBusy();
    initSaved();
}

void BackupAccount::doConnect() {
    connectPassword();
    connectSaved();
    connect(m_account, &backend::IAccountBackend::backupCreated, this,
            &BackupAccount::onBackupCreated, qontrol::UNIQUE);
    connect(m_account, &backend::IService::error, this, &BackupAccount::onServiceError,
            qontrol::UNIQUE);
}

void BackupAccount::view() {
    m_password = viewPassword();
    viewBusy();
    m_saved = viewSaved();
    hideBusy();
    hideSaved();
    retranslateUi();
    showPassword();
}

void BackupAccount::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void BackupAccount::retranslateUi() {
    setWindowTitle(TR("backup-title"));
    m_password_text->setText(TR("backup-password-text"));
    m_password_fields->setFieldLabel(m_password_input, TR("keys-password-field"));
    m_password_fields->setFieldLabel(m_confirm_input, TR("keys-password-confirm-field"));
    m_password_short->setText(TR("keys-password-short"));
    m_password_mismatch->setText(TR("keys-password-no-match"));
    m_busy->setTitle(TR("backup-sealing"));
    m_busy->setSubtitle(TR("backup-sealing-sub"));
}

// ===== Password view =====

void BackupAccount::initPassword() {
    m_password_text = new catalog::Label(catalog::LabelRole::Body);
    m_password_text->setTone(catalog::LabelTone::Secondary);
    m_password_text->setWordWrap(true);
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_confirm_input = new catalog::Input;
    m_confirm_input->setEchoMode(QLineEdit::Password);
    m_confirm_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_short =
        new catalog::FieldNote(TR("keys-password-short"), catalog::FieldNote::Tone::Error);
    m_password_mismatch =
        new catalog::FieldNote(TR("keys-password-no-match"), catalog::FieldNote::Tone::Error);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_save = catalog::btnSaveBackup();
    m_password_actions = new catalog::ModalActions;
    m_password_actions->addAction(m_save);
    m_password_fields = new catalog::FieldGroup;
}

void BackupAccount::connectPassword() {
    for (auto *input : {m_password_input, m_confirm_input}) {
        connect(input, &QLineEdit::textEdited, this, &BackupAccount::onPasswordEdited,
                qontrol::UNIQUE);
    }
    connect(m_confirm_input, &QLineEdit::returnPressed, this, &BackupAccount::onNextClicked,
            qontrol::UNIQUE);
    connect(m_save, &QPushButton::clicked, this, &BackupAccount::onNextClicked, qontrol::UNIQUE);
}

auto BackupAccount::viewPassword() -> QWidget * {
    m_password_fields->addField(TR("keys-password-field"), m_password_input)
        ->addNote(m_password_short)
        ->addField(TR("keys-password-confirm-field"), m_confirm_input)
        ->addNote(m_password_mismatch);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_password_text)
        ->push(m_password_fields)
        ->push(m_password_error);
}

void BackupAccount::showPassword() {
    m_view = View::Password;
    onPasswordEdited();
    setHeader(nullptr);
    setBody(m_password);
    setFooter(m_password_actions);
    setWidth(ModalWidth::M);
    m_password_input->setFocus();
}

void BackupAccount::hidePassword() {
    keep(m_password);
    keep(m_password_actions);
}

void BackupAccount::sealBackup() {
    if (!m_save->isEnabled() || m_account == nullptr) {
        return;
    }
    auto password = m_password_input->text().toUtf8();
    clearPassword();
    m_password_error->setVisible(false);
    showBusy();
    m_backup_req = nextRequestId();
    m_account->backup(m_backup_req, password);
    password.fill('\0');
}

void BackupAccount::clearPassword() {
    m_password_input->clear();
    m_confirm_input->clear();
}

// ===== Busy view =====

void BackupAccount::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

auto BackupAccount::viewBusy() -> QWidget * {
    return m_busy;
}

void BackupAccount::showBusy() {
    m_view = View::Busy;
    setHeader(nullptr);
    setBody(m_busy);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

void BackupAccount::hideBusy() {
    keep(m_busy);
}

// ===== Saved view =====

void BackupAccount::initSaved() {
    m_saved_notice = new catalog::Notice(catalog::Notice::Tone::Success);
    m_done = catalog::btnDone();
    m_saved_actions = new catalog::ModalActions;
    m_saved_actions->addAction(m_done);
}

void BackupAccount::connectSaved() {
    connect(m_done, &QPushButton::clicked, this, &BackupAccount::onNextClicked, qontrol::UNIQUE);
}

auto BackupAccount::viewSaved() -> QWidget * {
    return (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_saved_notice);
}

void BackupAccount::showSaved(const QString &path) {
    m_view = View::Saved;
    m_saved_notice->setText(TR("backup-saved").arg(QDir::toNativeSeparators(path)));
    setHeader(nullptr);
    setBody(m_saved);
    setFooter(m_saved_actions);
    setWidth(ModalWidth::M);
}

void BackupAccount::hideSaved() {
    keep(m_saved);
    keep(m_saved_actions);
}

// ===== Slots =====

void BackupAccount::onNextClicked() {
    switch (m_view) {
    case View::Password:
        sealBackup();
        break;
    case View::Busy:
        break;
    case View::Saved:
        accept();
        break;
    }
}

void BackupAccount::onPasswordEdited() {
    QString password = m_password_input->text();
    bool tooShort = !password.isEmpty() && password.size() < MIN_PASSWORD_LENGTH;
    bool mismatch = !m_confirm_input->text().isEmpty() && m_confirm_input->text() != password;
    m_password_short->setVisible(tooShort);
    m_password_mismatch->setVisible(mismatch);
    QString reason;
    if (password.isEmpty()) {
        reason = TR("keys-password-choose");
    } else if (password.size() < MIN_PASSWORD_LENGTH) {
        reason = TR("keys-password-short");
    } else if (m_confirm_input->text() != password) {
        reason = TR("keys-password-must-match");
    }
    m_save->setEnabled(reason.isEmpty());
    m_password_actions->setReason(reason);
}

void BackupAccount::onBackupCreated(backend::RequestId request_id, const QByteArray &backup) {
    if (request_id != m_backup_req) {
        return;
    }
    m_backup_req = 0;
    auto name = m_account == nullptr ? QString() : m_account->name();
    auto path = QFileDialog::getSaveFileName(this, TR("backup-save-title"),
                                             QDir::home().filePath(backupFileName(name)),
                                             TR("backup-file-filter"));
    if (path.isEmpty()) {
        showPassword();
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += BACKUP_SUFFIX;
    }
    auto error = writeBackupFile(path, backup);
    if (!error.isEmpty()) {
        m_password_error->setText(TR("backup-save-failed").arg(error));
        m_password_error->setVisible(true);
        showPassword();
        return;
    }
    showSaved(path);
}

void BackupAccount::onServiceError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0 || request_id != m_backup_req) {
        return;
    }
    m_backup_req = 0;
    m_password_error->setText(TR("backup-failed").arg(message));
    m_password_error->setVisible(true);
    showPassword();
}

} // namespace account::view
