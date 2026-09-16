#pragma once

#include <QString>

#include <Qontrol>
#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Button;
class Checkbox;
class ComboBox;
class Input;
class Label;
class ModalStatus;
class SelectRow;
class TextEdit;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

// Generates or imports a BIP39 mnemonic and registers it as a hot key. No xprv import
// and no BIP39 passphrase: a hot key is a mnemonic sealed with a password, and the
// password is taken twice here because this is where the secret is created.
class HotKey final : public qontrol::Modal {
    Q_OBJECT

public:
    enum class Mode { Generate, Import };
    enum class Step { Pick, Backup, Working };

    explicit HotKey(KeysModule *module, QWidget *parent = nullptr);
    ~HotKey() override;

signals:
    void keyAdded(QString fingerprint);

public slots:
    void onModeClicked();
    void onGenerateClicked();
    void onBackupToggled(bool checked);
    void onContinueClicked();
    void onPasswordEntered(QString password);
    void onMnemonicGenerated(ReqId req_id, MnemonicResult result);
    void onHotKeyImported(ReqId req_id, KeyInfoResult result);

protected:
    void init();
    void doConnect();
    void view();

    void applyStep();
    void showError(const QString &message);
    void clearSecrets();

private:
    KeysModule *m_module = nullptr;
    Mode m_mode = Mode::Generate;
    Step m_step = Step::Pick;
    QString m_mnemonic;
    // The id of the outstanding generateMnemonic()/importHotKey() request, so the
    // matching slot can ignore an answer meant for an earlier one.
    ReqId m_mnemonic_req_id = 0;
    ReqId m_import_req_id = 0;
    catalog::Input *m_alias_input = nullptr;
    catalog::ComboBox *m_word_count = nullptr;
    catalog::TextEdit *m_mnemonic_input = nullptr;
    catalog::Label *m_mnemonic_label = nullptr;
    catalog::Checkbox *m_backup_check = nullptr;
    catalog::Label *m_error_label = nullptr;
    catalog::ModalStatus *m_status = nullptr;
    catalog::Button *m_generate_btn = nullptr;
    catalog::Button *m_continue_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    catalog::SelectRow *m_generate_tab = nullptr;
    catalog::SelectRow *m_import_tab = nullptr;
    QWidget *m_generate_row = nullptr;
    QWidget *m_import_row = nullptr;
    QWidget *m_backup_row = nullptr;
};

} // namespace keys::modal
