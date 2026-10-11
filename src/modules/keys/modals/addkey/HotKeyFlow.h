#pragma once

#include <QString>

#include "AddKeyFlow.h"

#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Button;
class Checkbox;
class ComboBox;
class FieldGroup;
class FieldNote;
class Input;
class ModalActions;
class ModalStatus;
class SelectRow;
class WordGrid;
class WordInputGrid;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

// Adds a hot key: generates a mnemonic to back up or imports typed words, then seals
// it with a password.
class HotKeyFlow final : public AddKeyFlow {
    Q_OBJECT

public:
    explicit HotKeyFlow(KeysModule *module);
    ~HotKeyFlow() override;
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onHotKindClicked();
    void onWordCountPicked();
    void onHotChooseChanged();
    void onMnemonicGenerated(ReqId req_id, const MnemonicResult &result);
    void onBackupChanged();
    void onHotPasswordChanged();
    void onHotKeyImported(ReqId req_id, const KeyInfoResult &result);
    void onFailureRetry();

protected:
    enum class HotKind { Generate12, Generate24, Import };
    // The view on display: Back and the primary action act on it.
    enum class View { HotChoose, Backup, HotPassword, Sealing, Result };

    void init();
    void doConnect();
    void view();

    // Hot key choice view
    void initHotChoose();
    void connectHotChoose();
    [[nodiscard]] auto viewHotChoose() -> QWidget *;
    void showHotChoose();
    [[nodiscard]] auto typedMnemonic() const -> QString;
    void continueHot();

    // Backup view
    void initBackup();
    void connectBackup();
    [[nodiscard]] auto viewBackup() -> QWidget *;
    void showBackup();
    void hideBackup();

    // Hot key password view
    void initHotPassword();
    void connectHotPassword();
    [[nodiscard]] auto viewHotPassword() -> QWidget *;
    void showHotPassword();
    void hideHotPassword();
    void sealKey();

    // Status view
    void initStatus();
    void connectStatus();
    void showSealing();
    void showResult();
    void hideStatus();
    void showFailure(const QString &title, const QString &message);

    // Shared
    void clearSecrets();

private:
    KeysModule *m_module = nullptr;
    View m_view = View::HotChoose;

    HotKind m_hot_kind = HotKind::Generate12;
    catalog::SelectRow *m_generate12 = nullptr;
    catalog::SelectRow *m_generate24 = nullptr;
    catalog::SelectRow *m_import = nullptr;
    catalog::ComboBox *m_word_count = nullptr;
    catalog::WordInputGrid *m_words_input = nullptr;
    catalog::Button *m_choose_continue = nullptr;
    catalog::ModalActions *m_choose_actions = nullptr;
    QWidget *m_choose_body = nullptr;
    // The generated or typed mnemonic, until it is sealed or the flow is deleted.
    QString m_mnemonic;
    ReqId m_generate_req = 0;

    catalog::WordGrid *m_backup_words = nullptr;
    catalog::Checkbox *m_backup_check = nullptr;
    catalog::Button *m_backup_continue = nullptr;
    catalog::ModalActions *m_backup_actions = nullptr;
    QWidget *m_backup_body = nullptr;

    catalog::FieldGroup *m_password_fields = nullptr;
    catalog::Input *m_hot_alias = nullptr;
    catalog::Input *m_password = nullptr;
    catalog::FieldNote *m_password_short = nullptr;
    catalog::Input *m_confirm = nullptr;
    catalog::FieldNote *m_password_mismatch = nullptr;
    catalog::Button *m_seal = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    QWidget *m_password_body = nullptr;
    ReqId m_seal_req = 0;

    catalog::ModalStatus *m_status = nullptr;
    catalog::Button *m_done = nullptr;
    catalog::Button *m_try_again = nullptr;
    catalog::ModalActions *m_status_actions = nullptr;
};

} // namespace keys::modal
