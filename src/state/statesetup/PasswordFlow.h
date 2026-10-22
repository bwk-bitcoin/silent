#pragma once

#include "state/StateSetup.h"

#include <cstdint>
#include <memory>

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

namespace state {

class PasswordFlow final : public StateSetupFlow {
    Q_OBJECT

public:
    PasswordFlow();
    ~PasswordFlow() override;
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onPasswordEdited();
    void onPasswordCreated();

protected:
    enum class View : std::uint8_t { Password, Busy };

    void init();
    void doConnect();
    void view();

    // Password view
    void initPassword();
    void connectPassword();
    [[nodiscard]] auto viewPassword() -> QWidget *;
    void showPassword();
    void hidePassword();
    void createPassword();
    void clearPassword();

    // Busy view
    void initBusy();
    [[nodiscard]] auto viewBusy() -> catalog::ModalStatus *;
    void showBusy();
    void hideBusy();

    // Shared
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    View m_view = View::Password;
    QWidget *m_password = nullptr;
    catalog::Label *m_password_text = nullptr;
    catalog::FieldGroup *m_password_fields = nullptr;
    catalog::Input *m_password_input = nullptr;
    catalog::Input *m_confirm_input = nullptr;
    catalog::FieldNote *m_password_short = nullptr;
    catalog::FieldNote *m_password_mismatch = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    catalog::Button *m_create = nullptr;
    catalog::ModalStatus *m_busy = nullptr;
    std::shared_ptr<QString> m_result;
};

} // namespace state
