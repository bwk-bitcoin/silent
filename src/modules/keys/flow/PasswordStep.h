#pragma once

#include <QString>
#include <QWidget>

namespace catalog {
class Button;
class Input;
class Label;
class ModalActions;
class Notice;
} // namespace catalog

namespace keys::flow {

// Takes the password of a hot key: a prompt, the field, a wrong password reported in
// place, and the proceed action, which reads "Unlocking…" while busy. The value is
// handed over once and cleared; a QString cannot be zeroized, so this is best effort.
class PasswordStep : public QWidget {
    Q_OBJECT

public:
    // `proceed` is the step's own action variant (sign, check, get descriptor).
    explicit PasswordStep(catalog::Button *proceed, QWidget *parent = nullptr);
    ~PasswordStep() override;
    void setPrompt(const QString &prompt);
    // Shows Back on the left of the actions, for a step that has somewhere to return.
    void setBackVisible(bool visible);
    void showError(const QString &error);
    void setBusy(bool busy);
    // Back to an empty field, no error, not busy, with the focus in the field.
    void reset();
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void passwordEntered(QString password);
    void backClicked();

public slots:
    void onTextEdited();
    void onProceedClicked();

protected:
    void init(catalog::Button *proceed);
    void doConnect();
    void view();
    void updateActions();

private:
    catalog::Label *m_prompt = nullptr;
    catalog::Input *m_input = nullptr;
    catalog::Notice *m_error = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_proceed = nullptr;
    catalog::Button *m_unlocking = nullptr;
    bool m_busy = false;
};

} // namespace keys::flow
