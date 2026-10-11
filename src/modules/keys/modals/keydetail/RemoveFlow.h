#pragma once

#include <QString>

#include "modules/keys/modals/KeyDetail.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
} // namespace catalog

namespace keys::modal {

// Removes the key once confirmed, warning about the accounts using it and a hot key's
// secret going with it.
class RemoveFlow final : public KeyDetailFlow {
    Q_OBJECT

public:
    RemoveFlow(KeysModule *module, QString fingerprint);
    // Asks to confirm.
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();

protected:
    void init();
    void doConnect();
    void view();

    // Remove view
    void initRemove();
    void connectRemove();
    [[nodiscard]] auto viewRemove() -> QWidget *;
    void showRemove();
    void hideRemove();
    void removeKey();

private:
    catalog::Label *m_remove_question = nullptr;
    catalog::Notice *m_remove_accounts = nullptr;
    catalog::Notice *m_remove_hot = nullptr;
    catalog::Notice *m_remove_error = nullptr;
    catalog::Button *m_remove_confirm = nullptr;
    catalog::ModalActions *m_remove_actions = nullptr;
    QWidget *m_remove_body = nullptr;
};

} // namespace keys::modal
