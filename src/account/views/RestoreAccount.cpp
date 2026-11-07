#include "account/views/RestoreAccount.h"

#include "account/RequestId.h"
#include "account/Services.h"
#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/Section.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "theme/Palette.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <Qontrol>
#include <common.h>

namespace account::view {

RestoreAccount::RestoreAccount(backend::IAccountBackend *target) : m_target(target) {
    target->setParent(this);
    init();
    doConnect();
    view();
}

RestoreAccount::~RestoreAccount() {
    clearPassword();
}

void RestoreAccount::init() {
    initPassword();
    initBusy();
    initReview();
    initRestored();
}

void RestoreAccount::doConnect() {
    connectPassword();
    connectReview();
    connectRestored();
    connect(m_target, &backend::IService::error, this, &RestoreAccount::onServiceError,
            qontrol::UNIQUE);
}

void RestoreAccount::view() {
    m_password_view = viewPassword();
    viewBusy();
    m_review_view = viewReview();
    m_restored_view = viewRestored();
    hideBusy();
    hideReview();
    hideRestored();
    retranslateUi();
    showPassword();
}

void RestoreAccount::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void RestoreAccount::retranslateUi() {
    setWindowTitle(TR("restore-title"));
    m_password_text->setText(TR("restore-password-text"));
    m_password_fields->setFieldLabel(m_file_row, TR("restore-file-field"));
    m_password_fields->setFieldLabel(m_password_input, TR("keys-password-field"));
    if (m_backup_path.isEmpty()) {
        m_file_label->setText(TR("restore-no-file"));
    }
}

// ===== Password view =====

void RestoreAccount::initPassword() {
    m_password_text = new catalog::Label(catalog::LabelRole::Body);
    m_password_text->setTone(catalog::LabelTone::Secondary);
    m_password_text->setWordWrap(true);
    m_file_label = new catalog::Label(catalog::LabelRole::Body);
    m_file_label->setTone(catalog::LabelTone::Secondary);
    m_choose = catalog::btnChooseFile();
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_read = catalog::btnReadBackup();
    m_password_actions = new catalog::ModalActions;
    m_password_actions->addAction(m_read);
    m_password_fields = new catalog::FieldGroup;
}

void RestoreAccount::connectPassword() {
    connect(m_choose, &QPushButton::clicked, this, &RestoreAccount::onChooseClicked,
            qontrol::UNIQUE);
    connect(m_password_input, &QLineEdit::textEdited, this, &RestoreAccount::onPasswordEdited,
            qontrol::UNIQUE);
    connect(m_password_input, &QLineEdit::returnPressed, this, &RestoreAccount::onNextClicked,
            qontrol::UNIQUE);
    connect(m_read, &QPushButton::clicked, this, &RestoreAccount::onNextClicked, qontrol::UNIQUE);
    connect(m_target, &backend::IAccountBackend::backupInspected, this,
            &RestoreAccount::onBackupInspected, qontrol::UNIQUE);
}

auto RestoreAccount::viewPassword() -> QWidget * {
    m_file_row =
        (new qontrol::Row)->spacing(resolve(Spacing::S))->push(m_file_label, 1)->push(m_choose);
    m_password_fields->addField(TR("restore-file-field"), m_file_row)
        ->addField(TR("keys-password-field"), m_password_input);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_password_text)
        ->push(m_password_fields)
        ->push(m_password_error);
}

void RestoreAccount::showPassword() {
    m_view = View::Password;
    onPasswordEdited();
    setHeader(nullptr);
    setBody(m_password_view);
    setFooter(m_password_actions);
    setWidth(ModalWidth::M);
}

void RestoreAccount::hidePassword() {
    keep(m_password_view);
    keep(m_password_actions);
}

void RestoreAccount::readBackup() {
    if (!m_read->isEnabled() || m_target == nullptr) {
        return;
    }
    m_password = m_password_input->text().toUtf8();
    m_password_input->clear();
    m_password_error->setVisible(false);
    showBusy(TR("restore-reading"));
    m_read_req = nextRequestId();
    m_target->inspectBackup(m_read_req, m_backup, m_password);
}

void RestoreAccount::clearPassword() {
    m_password_input->clear();
    m_password.fill('\0');
    m_password.clear();
}

// ===== Busy view =====

void RestoreAccount::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

auto RestoreAccount::viewBusy() -> QWidget * {
    return m_busy;
}

void RestoreAccount::showBusy(const QString &title) {
    m_view = View::Busy;
    m_busy->setTitle(title);
    m_busy->setSubtitle(TR("restore-busy-sub"));
    setHeader(nullptr);
    setBody(m_busy);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

void RestoreAccount::hideBusy() {
    keep(m_busy);
}

// ===== Review view =====

void RestoreAccount::initReview() {
    m_review_account = new catalog::Label(catalog::LabelRole::Body);
    m_review_account->setWordWrap(true);
    m_review_lists = new qontrol::Column;
    m_review_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_review_error->setVisible(false);
    m_restore = catalog::btnRestoreAccount();
    m_review_actions = new catalog::ModalActions;
    m_review_actions->setBack(catalog::ModalActions::Back::Back);
    m_review_actions->addAction(m_restore);
}

void RestoreAccount::connectReview() {
    connect(m_review_actions, &catalog::ModalActions::backClicked, this,
            &RestoreAccount::onBackClicked, qontrol::UNIQUE);
    connect(m_restore, &QPushButton::clicked, this, &RestoreAccount::onNextClicked,
            qontrol::UNIQUE);
    connect(m_target, &backend::IAccountBackend::restored, this, &RestoreAccount::onRestored,
            qontrol::UNIQUE);
}

auto RestoreAccount::viewReview() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_review_account)
        ->push(m_review_lists)
        ->push(m_review_error);
}

void RestoreAccount::showReview(const backend::BackupInfo &info) {
    m_view = View::Review;
    m_account = info.account;
    auto network = info.network.has_value() ? networkName(*info.network) : QString();
    m_review_account->setText(
        TR("restore-review-account").arg(info.account, info.backend, network));
    catalog::clearRows(m_review_lists);
    m_review_lists->spacing(resolve(Spacing::M));
    m_review_lists->push(reviewList(TR("restore-includes"), info.includes));
    m_review_lists->push(reviewList(TR("restore-requirements"), info.requirements));
    for (const auto &warning : info.warnings) {
        auto *notice = new catalog::Notice(catalog::Notice::Tone::Warning);
        notice->setText(warning);
        m_review_lists->push(notice);
    }
    m_review_error->setVisible(false);
    setHeader(nullptr);
    setBody(m_review_view);
    setFooter(m_review_actions);
    setWidth(ModalWidth::M);
}

void RestoreAccount::hideReview() {
    keep(m_review_view);
    keep(m_review_actions);
}

void RestoreAccount::restoreBackup() {
    if (m_target == nullptr) {
        return;
    }
    showBusy(TR("restore-restoring"));
    m_restore_req = nextRequestId();
    m_target->restore(m_restore_req, m_backup, m_password);
}

auto RestoreAccount::reviewList(const QString &title, const QStringList &lines) -> QWidget * {
    auto *list = (new qontrol::Column)->spacing(resolve(Spacing::XS));
    for (const auto &line : lines) {
        auto *label = new catalog::Label(QStringLiteral("• ") + line);
        label->setWordWrap(true);
        list->push(label);
    }
    auto *section = new catalog::Section(title);
    section->setContent(list);
    return section;
}

// ===== Restored view =====

void RestoreAccount::initRestored() {
    m_restored_notice = new catalog::Notice(catalog::Notice::Tone::Success);
    m_done = catalog::btnDone();
    m_restored_actions = new catalog::ModalActions;
    m_restored_actions->addAction(m_done);
}

void RestoreAccount::connectRestored() {
    connect(m_done, &QPushButton::clicked, this, &RestoreAccount::onNextClicked, qontrol::UNIQUE);
}

auto RestoreAccount::viewRestored() -> QWidget * {
    return (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_restored_notice);
}

void RestoreAccount::showRestored() {
    m_view = View::Restored;
    m_restored_notice->setText(TR("restore-done").arg(m_account));
    setHeader(nullptr);
    setBody(m_restored_view);
    setFooter(m_restored_actions);
    setWidth(ModalWidth::M);
}

void RestoreAccount::hideRestored() {
    keep(m_restored_view);
    keep(m_restored_actions);
}

// ===== Slots =====

void RestoreAccount::onBackClicked() {
    switch (m_view) {
    case View::Review:
        clearPassword();
        showPassword();
        break;
    case View::Password:
    case View::Busy:
    case View::Restored:
        break;
    }
}

void RestoreAccount::onNextClicked() {
    switch (m_view) {
    case View::Password:
        readBackup();
        break;
    case View::Review:
        restoreBackup();
        break;
    case View::Restored:
        accept();
        break;
    case View::Busy:
        break;
    }
}

void RestoreAccount::onChooseClicked() {
    auto path = QFileDialog::getOpenFileName(this, TR("restore-open-title"), QDir::homePath(),
                                             TR("backup-file-filter"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_password_error->setText(TR("restore-open-failed").arg(file.errorString()));
        m_password_error->setVisible(true);
        refit();
        return;
    }
    m_backup = file.readAll();
    m_backup_path = path;
    m_file_label->setText(QFileInfo(path).fileName());
    m_password_error->setVisible(false);
    onPasswordEdited();
    refit();
}

void RestoreAccount::onPasswordEdited() {
    QString reason;
    if (m_backup_path.isEmpty()) {
        reason = TR("restore-choose-file");
    } else if (m_password_input->text().isEmpty()) {
        reason = TR("restore-enter-password");
    }
    m_read->setEnabled(reason.isEmpty());
    m_password_actions->setReason(reason);
}

void RestoreAccount::onBackupInspected(backend::RequestId request_id,
                                       const backend::BackupInfo &info) {
    if (request_id != m_read_req) {
        return;
    }
    m_read_req = 0;
    showReview(info);
}

void RestoreAccount::onRestored(backend::RequestId request_id) {
    if (request_id != m_restore_req) {
        return;
    }
    m_restore_req = 0;
    clearPassword();
    showRestored();
}

void RestoreAccount::onServiceError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0) {
        return;
    }
    // A failed read goes back to the password, a failed restore back to the review.
    if (request_id == m_read_req) {
        m_read_req = 0;
        clearPassword();
        m_password_error->setText(message);
        m_password_error->setVisible(true);
        showPassword();
    } else if (request_id == m_restore_req) {
        m_restore_req = 0;
        m_review_error->setText(TR("restore-failed").arg(message));
        m_review_error->setVisible(true);
        m_view = View::Review;
        setBody(m_review_view);
        setFooter(m_review_actions);
    }
}

} // namespace account::view
