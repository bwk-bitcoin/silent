#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/backend/types.h"

#include <QByteArray>
#include <QEvent>
#include <QPointer>
#include <QString>
#include <cstdint>

namespace backend {
class IAccountBackend;
} // namespace backend

namespace catalog {
class Button;
class FieldGroup;
class FieldNote;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
} // namespace catalog

namespace account::view {

// Writes `backup`, an account backup, to the file `path`, replacing it. The error, empty
// once written.
auto writeBackupFile(const QString &path, const QByteArray &backup) -> QString;
// The default file name of the backup of the account `name`.
auto backupFileName(const QString &name) -> QString;

// Backs the account up: a password and its confirmation, IAccountBackend::backup() seals
// the backup under it, then Save writes it to a file the user picks. The password is held
// only while the backup is sealed.
class BackupAccount final : public catalog::Modal {
    Q_OBJECT

public:
    explicit BackupAccount(backend::IAccountBackend *account);
    ~BackupAccount() override;

public slots:
    void onNextClicked();
    void onPasswordEdited();
    void onBackupCreated(backend::RequestId request_id, const QByteArray &backup);
    void onServiceError(backend::RequestId request_id, const QString &message);

protected:
    enum class View : std::uint8_t { Password, Busy, Saved };

    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

    // Password view
    void initPassword();
    void connectPassword();
    auto viewPassword() -> QWidget *;
    void showPassword();
    void hidePassword();
    void sealBackup();
    void clearPassword();

    // Busy view
    void initBusy();
    auto viewBusy() -> QWidget *;
    void showBusy();
    void hideBusy();

    // Saved view
    void initSaved();
    void connectSaved();
    auto viewSaved() -> QWidget *;
    void showSaved(const QString &path);
    void hideSaved();

private:
    QPointer<backend::IAccountBackend> m_account;
    View m_view = View::Password;
    backend::RequestId m_backup_req = 0;

    QWidget *m_password = nullptr;
    catalog::Label *m_password_text = nullptr;
    catalog::FieldGroup *m_password_fields = nullptr;
    catalog::Input *m_password_input = nullptr;
    catalog::Input *m_confirm_input = nullptr;
    catalog::FieldNote *m_password_short = nullptr;
    catalog::FieldNote *m_password_mismatch = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::Button *m_save = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;

    catalog::ModalStatus *m_busy = nullptr;

    QWidget *m_saved = nullptr;
    catalog::Notice *m_saved_notice = nullptr;
    catalog::Button *m_done = nullptr;
    catalog::ModalActions *m_saved_actions = nullptr;
};

} // namespace account::view
