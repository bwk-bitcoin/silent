#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/backend/types.h"
#include "interfaces/signer.h"
#include "interfaces/types.h"

#include <QByteArray>
#include <QEvent>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

class IModule;

namespace backend {
class IAccountBackend;
} // namespace backend

namespace catalog {
class Button;
class Checkbox;
class ComboBox;
class FieldGroup;
class FieldNote;
class FoldSection;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
class ScrollArea;
class ValidatedInput;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace account::view {

// Restores an account from its backup through `target`, a module's restore target (an
// IAccountBackend with no account behind it): the backup file, then what inspectBackup()
// reports it holds and needs, then restore(), which makes the account. An encrypted backup
// asks for a key first, as Liana's decrypt modal does: each connected signing device is
// listed and tried with its xpubs at the paths the backup names, and other options paste
// or import an xpub, or take a mnemonic. The keys that open the backup are held from then
// to the restore, never longer. `module` asks for the devices to be polled while they are
// listed, none when null; `device_name` names each one, as the keys module does (the shared
// account page reaches no module). The modal takes `target` as its child.
class RestoreAccount final : public catalog::Modal {
    Q_OBJECT

public:
    using DeviceName = QString (*)(const ISigner &signer);

    RestoreAccount(backend::IAccountBackend *target, IModule *module, DeviceName device_name);
    ~RestoreAccount() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onChooseClicked();
    void onBackupInspected(backend::RequestId request_id, const backend::BackupInfo &info);
    void onBackupLocked(backend::RequestId request_id, const QStringList &derivation_paths);
    void onRestored(backend::RequestId request_id);
    void onServiceError(backend::RequestId request_id, const QString &message);
    void onInstanceChanged(const QString &id);
    void onDeviceXpub(ReqId req_id, const QString &xpub);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message);
    void onXpubEdited();
    void onPasteXpubClicked();
    void onImportXpubClicked();
    void onMnemonicAckToggled(bool acknowledged);
    void onMnemonicEdited();
    void onNameEdited();

protected:
    enum class View : std::uint8_t { File, Busy, Decrypt, Review, Restored };
    // How a listed device's try goes.
    enum class DeviceStatus : std::uint8_t { Trying, Failed };

    // A connected device the backup is tried with: its xpubs at each path, then one read.
    struct DeviceTry {
        QPointer<ISigner> signer;
        QString name;
        QString fingerprint;
        DeviceStatus status = DeviceStatus::Trying;
        // The index of the path the device is asked next.
        int path = 0;
        QStringList xpubs;
        ReqId xpub_req = 0;
        backend::RequestId read_req = 0;
    };

    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

    // File view
    void initFile();
    void connectFile();
    auto viewFile() -> QWidget *;
    void showFile();
    void readBackup();

    // Busy view
    void initBusy();
    auto viewBusy() -> QWidget *;
    void showBusy(const QString &title);
    void hideBusy();

    // Decrypt view
    void initDecrypt();
    void connectDecrypt();
    auto viewDecrypt() -> catalog::ScrollArea *;
    void showDecrypt(const QStringList &derivation_paths);
    void hideDecrypt();
    void leaveDecrypt();
    // Lists the connected devices again, trying each new ready one.
    void refreshDevices();
    void tryDevice(const QString &instance_id, ISigner *signer);
    // Asks the device its xpub at the next path, or reads the backup with the ones it gave.
    void fetchNext(DeviceTry &device);
    // The device `signer` (the sender of its answer) whose request `req_id` is.
    auto deviceOf(const QObject *signer, ReqId req_id) -> DeviceTry *;
    auto deviceRead(backend::RequestId request_id) -> DeviceTry *;
    void stopDevices();
    void readWithXpub(const QString &xpub);
    void opened(const QStringList &xpubs, const QString &opener);

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

    // Shared
    void clearKeys();

private:
    QPointer<backend::IAccountBackend> m_target;
    IModule *m_module = nullptr;
    DeviceName m_device_name = nullptr;
    View m_view = View::File;
    backend::RequestId m_read_req = 0;
    backend::RequestId m_xpub_req = 0;
    backend::RequestId m_mnemonic_req = 0;
    backend::RequestId m_restore_req = 0;
    QByteArray m_backup;
    QString m_backup_path;
    // The paths of the xpubs that may open the backup.
    QStringList m_paths;
    // The devices tried, by instance id.
    QHash<QString, DeviceTry> m_devices;
    // The keys that opened the backup, from then to the restore.
    QStringList m_xpubs;
    QByteArray m_mnemonic;
    // Who opened it, said in the review: empty for a plain backup.
    QString m_opener;
    std::optional<backend::Network> m_network;
    QString m_account;

    QWidget *m_file_view = nullptr;
    catalog::Label *m_file_text = nullptr;
    catalog::FieldGroup *m_file_fields = nullptr;
    QWidget *m_file_row = nullptr;
    catalog::Label *m_file_label = nullptr;
    catalog::Button *m_choose = nullptr;
    catalog::Notice *m_file_error = nullptr;
    catalog::Button *m_read = nullptr;
    catalog::ModalActions *m_file_actions = nullptr;

    catalog::ModalStatus *m_busy = nullptr;

    QWidget *m_decrypt_view = nullptr;
    catalog::Label *m_decrypt_text = nullptr;
    qontrol::Column *m_device_rows = nullptr;
    catalog::FoldSection *m_options = nullptr;
    catalog::Label *m_airgap_text = nullptr;
    catalog::Label *m_paths_label = nullptr;
    catalog::FieldGroup *m_option_fields = nullptr;
    QWidget *m_xpub_row = nullptr;
    catalog::ValidatedInput *m_xpub_input = nullptr;
    catalog::Button *m_paste_xpub = nullptr;
    catalog::FieldNote *m_xpub_note = nullptr;
    catalog::Button *m_import_xpub = nullptr;
    catalog::Notice *m_mnemonic_warning = nullptr;
    catalog::Checkbox *m_mnemonic_ack = nullptr;
    catalog::FieldGroup *m_mnemonic_fields = nullptr;
    catalog::ValidatedInput *m_mnemonic_input = nullptr;
    catalog::FieldNote *m_mnemonic_note = nullptr;
    catalog::ModalActions *m_decrypt_actions = nullptr;

    QWidget *m_review_view = nullptr;
    catalog::Notice *m_review_opened = nullptr;
    catalog::Label *m_review_account = nullptr;
    catalog::FieldGroup *m_review_fields = nullptr;
    catalog::Input *m_name_input = nullptr;
    catalog::ComboBox *m_network_combo = nullptr;
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
