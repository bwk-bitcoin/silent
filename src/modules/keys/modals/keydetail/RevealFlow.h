#pragma once

#include <QString>

#include "modules/keys/modals/KeyDetail.h"

#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Button;
class Input;
class ModalActions;
class Notice;
class WordGrid;
} // namespace catalog

namespace keys::modal {

// Reveals a hot key's mnemonic: asks the key's password, then shows the words under a
// warning, with a toggle to mask them.
class RevealFlow final : public KeyDetailFlow {
    Q_OBJECT

public:
    RevealFlow(KeysModule *module, QString fingerprint);
    ~RevealFlow() override;
    // Asks the password.
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onPasswordEdited();
    void onMnemonicRevealed(ReqId req_id, const MnemonicResult &result);
    void onToggleWordsClicked();

protected:
    // The view on display: Back and the primary action act on it.
    enum class View { Password, Reveal };

    void init();
    void doConnect();
    void view();

    // Password view
    void initPassword();
    void connectPassword();
    [[nodiscard]] auto viewPassword() -> QWidget *;
    void showPassword();
    void proceedPassword();

    // Reveal view
    void initReveal();
    void connectReveal();
    [[nodiscard]] auto viewReveal() -> QWidget *;
    void showReveal();
    void hideReveal();
    void closeWords();

private:
    View m_view = View::Password;

    // The value is handed over once and cleared; a QString cannot be zeroized, so this
    // is best effort.
    catalog::Input *m_password_input = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::Button *m_password_proceed = nullptr;
    catalog::Button *m_password_unlocking = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    QWidget *m_password_body = nullptr;
    // The outstanding reveal request, so a stale answer is ignored.
    ReqId m_reveal_req = 0;
    catalog::WordGrid *m_words = nullptr;
    catalog::Button *m_hide_words = nullptr;
    catalog::Button *m_show_words = nullptr;
    catalog::Button *m_words_done = nullptr;
    catalog::ModalActions *m_reveal_actions = nullptr;
    QWidget *m_reveal_body = nullptr;
};

} // namespace keys::modal
