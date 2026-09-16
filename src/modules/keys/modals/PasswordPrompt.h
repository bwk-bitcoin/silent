#pragma once

#include <QString>

#include <Qontrol>

namespace catalog {
class Button;
class Input;
class Label;
} // namespace catalog

namespace keys::modal {

// Takes a password: twice when a secret is being created, once when one is opened. The
// value lives in this modal for exactly as long as it takes to hand it over. A QString
// cannot be zeroized, so this is best effort: Rust owns the real wiping, and the
// password is consumed by a single call there.
class PasswordPrompt final : public qontrol::Modal {
    Q_OBJECT

public:
    enum class Mode { Use, Create };

    explicit PasswordPrompt(Mode mode, QString title, QWidget *parent = nullptr);
    ~PasswordPrompt() override;

    [[nodiscard]] auto password() const -> QString;
    void clearPassword();

signals:
    // Not "accepted": QDialog already has a signal by that name.
    void passwordEntered(QString password);

public slots:
    void onTextChanged();
    void onConfirmClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    Mode m_mode = Mode::Use;
    QString m_title;
    QString m_password;
    catalog::Input *m_password_input = nullptr;
    catalog::Input *m_confirm_input = nullptr;
    QWidget *m_confirm_row = nullptr;
    catalog::Label *m_error_label = nullptr;
    catalog::Button *m_confirm_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
};

} // namespace keys::modal
