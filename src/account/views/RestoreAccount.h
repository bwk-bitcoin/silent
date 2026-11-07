#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/backend/types.h"

#include <QByteArray>
#include <QEvent>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstdint>

namespace backend {
class IAccountBackend;
} // namespace backend

namespace catalog {
class Button;
class FieldGroup;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace account::view {

// Restores an account from its backup through `target`, a module's restore target (an
// IAccountBackend with no account behind it): the backup file and its password, then what
// inspectBackup() reports it holds and needs, then restore(), which makes the account.
// The backup is opaque: the modal only hands its bytes over. The password is held from the
// read to the restore, never longer. The modal takes `target` as its child.
class RestoreAccount final : public catalog::Modal {
    Q_OBJECT

public:
    explicit RestoreAccount(backend::IAccountBackend *target);
    ~RestoreAccount() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onChooseClicked();
    void onPasswordEdited();
    void onBackupInspected(backend::RequestId request_id, const backend::BackupInfo &info);
    void onRestored(backend::RequestId request_id);
    void onServiceError(backend::RequestId request_id, const QString &message);

protected:
    enum class View : std::uint8_t { Password, Busy, Review, Restored };

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
    void readBackup();
    void clearPassword();

    // Busy view
    void initBusy();
    auto viewBusy() -> QWidget *;
    void showBusy(const QString &title);
    void hideBusy();

    // Review view
    void initReview();
    void connectReview();
    auto viewReview() -> QWidget *;
    void showReview(const backend::BackupInfo &info);
    void hideReview();
    void restoreBackup();
    // A titled list of `lines`, one bullet each.
    static auto reviewList(const QString &title, const QStringList &lines) -> QWidget *;

    // Restored view
    void initRestored();
    void connectRestored();
    auto viewRestored() -> QWidget *;
    void showRestored();
    void hideRestored();

private:
    QPointer<backend::IAccountBackend> m_target;
    View m_view = View::Password;
    backend::RequestId m_read_req = 0;
    backend::RequestId m_restore_req = 0;
    QByteArray m_backup;
    QString m_backup_path;
    // From the read to the restore.
    QByteArray m_password;
    QString m_account;

    QWidget *m_password_view = nullptr;
    catalog::Label *m_password_text = nullptr;
    catalog::FieldGroup *m_password_fields = nullptr;
    QWidget *m_file_row = nullptr;
    catalog::Label *m_file_label = nullptr;
    catalog::Button *m_choose = nullptr;
    catalog::Input *m_password_input = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::Button *m_read = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;

    catalog::ModalStatus *m_busy = nullptr;

    QWidget *m_review_view = nullptr;
    catalog::Label *m_review_account = nullptr;
    qontrol::Column *m_review_lists = nullptr;
    catalog::Notice *m_review_error = nullptr;
    catalog::Button *m_restore = nullptr;
    catalog::ModalActions *m_review_actions = nullptr;

    QWidget *m_restored_view = nullptr;
    catalog::Notice *m_restored_notice = nullptr;
    catalog::Button *m_done = nullptr;
    catalog::ModalActions *m_restored_actions = nullptr;
};

} // namespace account::view
