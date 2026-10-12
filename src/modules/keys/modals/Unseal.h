#pragma once

#include "catalog/containers/Modal.h"

#include <QElapsedTimer>
#include <QString>

#include <cstdint>

class QTimer;

namespace catalog {
class Button;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
} // namespace catalog

namespace keys::modal {

// The one password prompt that unseals a hot key's mnemonic, opened on top of whatever
// needs it: revealing the words, signing, fetching a descriptor or an xpub. It hands the
// password over and works under a spinner, shown at least BUSY_MIN_MS, until the caller
// says how it went: finish() closes it, retry() asks again with the reason.
class Unseal final : public catalog::Modal {
    Q_OBJECT

public:
    // `proceed` names what the password does (Sign, Show Mnemonic...), owned from here.
    Unseal(const QString &title, const QString &prompt, catalog::Button *proceed);
    ~Unseal() override;
    // The spinner's lines while the caller works.
    void setBusy(const QString &title, const QString &subtitle);
    // Back on the password step, closing as the header cross does; shown by default.
    void setBackShown(bool shown);
    // Closes, accepted or not, once the spinner has shown long enough.
    void finish(bool accepted);
    // Back to the password, `error` under it, once the spinner has shown long enough.
    void retry(const QString &error);

signals:
    void passwordEntered(const QString &password);

public slots:
    void onNextClicked();
    void onPasswordEdited();
    void onBusyHeld();

protected:
    // The view on display: the primary action acts on it.
    enum class View : std::uint8_t { Password, Busy };
    // What the caller asked for, held until the spinner has shown long enough.
    enum class Outcome : std::uint8_t { None, Accept, Reject, Retry };

    void init();
    void doConnect();
    void view();

    // Password view
    void initPassword();
    [[nodiscard]] auto viewPassword() -> QWidget *;
    void showPassword();
    void proceedPassword();

    // Busy view
    void initBusy();
    void showBusy();
    void hideBusy();
    void hold(Outcome outcome);
    void apply();

private:
    View m_view = View::Password;
    QString m_title;

    QString m_prompt_text;
    catalog::Label *m_prompt = nullptr;
    catalog::Input *m_password_input = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::Button *m_proceed = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    QWidget *m_password_step = nullptr;

    catalog::ModalStatus *m_busy_step = nullptr;
    QElapsedTimer m_busy_shown;
    QTimer *m_busy_hold = nullptr;
    Outcome m_outcome = Outcome::None;
    QString m_retry_error;
};

} // namespace keys::modal
