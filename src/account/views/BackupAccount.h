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

// Backs the account up: Save Backup asks IAccountBackend::backup() for the backup, which
// the account encrypts to its keys, then writes it to a file the user picks. The saved
// view names the keys that cannot open it.
class BackupAccount final : public catalog::Modal {
    Q_OBJECT

public:
    explicit BackupAccount(backend::IAccountBackend *account);

public slots:
    void onNextClicked();
    void onBackupCreated(backend::RequestId request_id, const QByteArray &backup,
                         const QStringList &excluded);
    void onServiceError(backend::RequestId request_id, const QString &message);

protected:
    enum class View : std::uint8_t { Save, Busy, Saved };

    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

    // Save view
    void initSave();
    void connectSave();
    auto viewSave() -> QWidget *;
    void showSave();
    void hideSave();
    void makeBackup();

    // Busy view
    void initBusy();
    auto viewBusy() -> QWidget *;
    void showBusy();
    void hideBusy();

    // Saved view
    void initSaved();
    void connectSaved();
    auto viewSaved() -> QWidget *;
    void showSaved(const QString &path, const QStringList &excluded);
    void hideSaved();

private:
    QPointer<backend::IAccountBackend> m_account;
    View m_view = View::Save;
    backend::RequestId m_backup_req = 0;

    QWidget *m_save_view = nullptr;
    catalog::Label *m_save_text = nullptr;
    catalog::Notice *m_save_error = nullptr;
    catalog::Button *m_save = nullptr;
    catalog::ModalActions *m_save_actions = nullptr;

    catalog::ModalStatus *m_busy = nullptr;

    QWidget *m_saved = nullptr;
    catalog::Notice *m_saved_notice = nullptr;
    // The keys the backup is not encrypted to.
    catalog::Notice *m_excluded_notice = nullptr;
    catalog::Button *m_done = nullptr;
    catalog::ModalActions *m_saved_actions = nullptr;
};

} // namespace account::view
