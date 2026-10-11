#pragma once

#include <optional>

#include <QString>

#include "modules/keys/KeysModule.h"
#include "modules/keys/modals/AddKey.h"

namespace catalog {
class Button;
class FieldNote;
class Input;
class ModalActions;
class Notice;
class ValidatedInput;
} // namespace catalog

namespace keys::modal {

// Adds a key from a pasted xpub with its origin, as an airgapped device exports it.
class PasteFlow final : public AddKeyFlow {
    Q_OBJECT

public:
    explicit PasteFlow(KeysModule *module);
    void start() override;

public slots:
    void onNextClicked();
    void onPasteChanged();

protected:
    struct PastedKey {
        QString fingerprint;
        PathXpub xpub;
    };

    void init();
    void doConnect();
    void view();
    void showPaste();
    [[nodiscard]] auto pastedKey() const -> std::optional<PastedKey>;

private:
    KeysModule *m_module = nullptr;
    catalog::ValidatedInput *m_paste = nullptr;
    catalog::FieldNote *m_paste_note = nullptr;
    catalog::Input *m_paste_alias = nullptr;
    catalog::Notice *m_paste_duplicate = nullptr;
    catalog::Notice *m_paste_error = nullptr;
    catalog::Button *m_add_pasted = nullptr;
    catalog::ModalActions *m_paste_actions = nullptr;
    QWidget *m_paste_body = nullptr;
};

} // namespace keys::modal
