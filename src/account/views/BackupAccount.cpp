#include "account/views/BackupAccount.h"

#include "account/RequestId.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "theme/Palette.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QRegularExpression>
#include <Qontrol>
#include <common.h>

namespace account::view {

namespace {

const char *const BACKUP_SUFFIX = ".bip138";

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

void BackupAccount::init() {
    initSave();
    initBusy();
    initSaved();
}

void BackupAccount::doConnect() {
    connectSave();
    connectSaved();
    connect(m_account, &backend::IAccountBackend::backupCreated, this,
            &BackupAccount::onBackupCreated, qontrol::UNIQUE);
    connect(m_account, &backend::IService::error, this, &BackupAccount::onServiceError,
            qontrol::UNIQUE);
}

void BackupAccount::view() {
    m_save_view = viewSave();
    viewBusy();
    m_saved = viewSaved();
    hideBusy();
    hideSaved();
    retranslateUi();
    showSave();
}

void BackupAccount::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void BackupAccount::retranslateUi() {
    setWindowTitle(TR("backup-title"));
    m_save_text->setText(m_account == nullptr ? QString() : m_account->backupDescription().details);
    m_busy->setTitle(TR("backup-encrypting"));
    m_busy->setSubtitle(TR("backup-encrypting-sub"));
}

// ===== Save view =====

void BackupAccount::initSave() {
    m_save_text = new catalog::Label(catalog::LabelRole::Body);
    m_save_text->setTone(catalog::LabelTone::Secondary);
    m_save_text->setWordWrap(true);
    m_save_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_save_error->setVisible(false);
    m_save = catalog::btnSaveBackup();
    m_save_actions = new catalog::ModalActions;
    m_save_actions->addAction(m_save);
}

void BackupAccount::connectSave() {
    connect(m_save, &QPushButton::clicked, this, &BackupAccount::onNextClicked, qontrol::UNIQUE);
}

auto BackupAccount::viewSave() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_save_text)
        ->push(m_save_error);
}

void BackupAccount::showSave() {
    m_view = View::Save;
    setHeader(nullptr);
    setBody(m_save_view);
    setFooter(m_save_actions);
    setWidth(ModalWidth::M);
}

void BackupAccount::hideSave() {
    keep(m_save_view);
    keep(m_save_actions);
}

void BackupAccount::makeBackup() {
    if (m_account == nullptr) {
        return;
    }
    m_save_error->setVisible(false);
    showBusy();
    m_backup_req = nextRequestId();
    m_account->backup(m_backup_req);
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
    m_excluded_notice = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_excluded_notice->setVisible(false);
    m_done = catalog::btnDone();
    m_saved_actions = new catalog::ModalActions;
    m_saved_actions->addAction(m_done);
}

void BackupAccount::connectSaved() {
    connect(m_done, &QPushButton::clicked, this, &BackupAccount::onNextClicked, qontrol::UNIQUE);
}

auto BackupAccount::viewSaved() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_saved_notice)
        ->push(m_excluded_notice);
}

void BackupAccount::showSaved(const QString &path, const QStringList &excluded) {
    m_view = View::Saved;
    m_saved_notice->setText(TR("backup-saved").arg(QDir::toNativeSeparators(path)));
    // BIP138 asks that the user knows which key cannot open the backup.
    m_excluded_notice->setText(TR("backup-excluded").arg(excluded.join(QStringLiteral(", "))));
    m_excluded_notice->setVisible(!excluded.isEmpty());
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
    case View::Save:
        makeBackup();
        break;
    case View::Busy:
        break;
    case View::Saved:
        accept();
        break;
    }
}

void BackupAccount::onBackupCreated(backend::RequestId request_id, const QByteArray &backup,
                                    const QStringList &excluded) {
    if (request_id != m_backup_req) {
        return;
    }
    m_backup_req = 0;
    auto name = m_account == nullptr ? QString() : m_account->name();
    auto path = QFileDialog::getSaveFileName(this, TR("backup-save-title"),
                                             QDir::home().filePath(backupFileName(name)),
                                             TR("backup-file-filter"));
    if (path.isEmpty()) {
        showSave();
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += BACKUP_SUFFIX;
    }
    auto error = writeBackupFile(path, backup);
    if (!error.isEmpty()) {
        m_save_error->setText(TR("backup-save-failed").arg(error));
        m_save_error->setVisible(true);
        showSave();
        return;
    }
    showSaved(path, excluded);
}

void BackupAccount::onServiceError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0 || request_id != m_backup_req) {
        return;
    }
    m_backup_req = 0;
    m_save_error->setText(TR("backup-failed").arg(message));
    m_save_error->setVisible(true);
    showSave();
}

} // namespace account::view
