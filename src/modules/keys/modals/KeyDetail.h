#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "catalog/containers/Modal.h"

#include <interfaces/types.h>
#include <silent.h>

class ISigner;

namespace catalog {
class Button;
class ComboBox;
class FieldNote;
class Input;
class Label;
class ModalActions;
class Notice;
class ScrollArea;
class Section;
class Table;
class WordGrid;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

// An account whose descriptor this key signs for.
struct KeyAccount {
    QString name;
    QString descriptor;
};

// Where an account's descriptor stands on the key's connected device.
enum class Registration { Unknown, Checking, Registering, Registered, NotRegistered };

// One registered key: its alias, the devices it was seen on, its routes in priority
// order, its cached xpubs and the accounts using it, with each account's descriptor
// checked and registered on the key's connected device. Revealing the mnemonic of
// a hot key and removing the key are steps of this same modal; fetching an xpub
// opens its own modal over it.
class KeyDetail final : public catalog::Modal {
    Q_OBJECT

public:
    KeyDetail(KeysModule *module, const QString &fingerprint);
    ~KeyDetail() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onKeyChanged();
    void onAliasEdited();
    void onSaveAliasClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onRemoveRouteClicked();
    void onAddRouteClicked();
    void onConfirmRouteClicked();
    void onCancelRouteClicked();
    void onFetchXpubClicked();
    void onRegisterClicked();
    void onDescriptorIsRegistered(ReqId req_id, bool registered);
    void onDescriptorRegistered(ReqId req_id, bool registered);
    void onRegistrationError(std::optional<ReqId> req_id, const QString &message);
    void onPasswordEdited();
    void onMnemonicRevealed(ReqId req_id, const MnemonicResult &result);
    void onToggleWordsClicked();

protected:
    // The view on display: Back and the primary action act on it.
    enum class View { Detail, Password, Reveal, Remove };

    void init();
    void doConnect();
    void view();

    // Detail view
    // Detail view
    void initDetail();
    void connectDetail();
    [[nodiscard]] auto viewDetail() -> catalog::ScrollArea *;
    void showDetail();
    void refreshDevices();
    void refreshRoutes(const KeyInfo &info);
    void moveRoute(int from_index, int to_index);
    void showRouteResult(const KeyOpResult &result);
    void refreshXpubs();
    void refreshAccounts(const KeyInfo &info);
    [[nodiscard]] auto accounts() const -> QList<KeyAccount>;
    [[nodiscard]] auto registrationCell(const KeyAccount &account, bool applies) -> QWidget *;
    // Follows the key's ready device: a new one is asked about every account, and
    // accounts added since are asked about on the same one.
    void syncRegistrations(const QList<KeyAccount> &accounts, bool applies);
    // Sends the next queued check or registration, one at a time on the device.
    void sendNextRegistration();
    void finishRegistrationRequest(Registration state);
    void stopRegistrations();
    void showRegistrationError(const QString &message);

    // Password view
    // Password view
    void initPassword();
    void connectPassword();
    [[nodiscard]] auto viewPassword() -> QWidget *;
    void showPassword();
    void hidePassword();
    void proceedPassword();

    // Reveal view
    // Reveal view
    void initReveal();
    void connectReveal();
    [[nodiscard]] auto viewReveal() -> QWidget *;
    void showReveal();
    void hideReveal();
    void closeWords();

    // Remove view
    // Remove view
    void initRemove();
    void connectRemove();
    [[nodiscard]] auto viewRemove() -> QWidget *;
    void showRemove();
    void hideRemove();
    void removeKey();

    // Shared
    // Shared
    [[nodiscard]] auto info() const -> std::optional<KeyInfo>;
    // The title on the left, the fingerprint and the key status on the right.
    void showHeader(const QString &title);

private:
    KeysModule *m_module = nullptr;
    View m_view = View::Detail;
    QString m_fingerprint;
    QString m_alias;

    catalog::Input *m_alias_input = nullptr;
    catalog::Button *m_save_alias = nullptr;
    catalog::FieldNote *m_alias_note = nullptr;
    catalog::Notice *m_route_error = nullptr;
    catalog::Section *m_devices_section = nullptr;
    catalog::Table *m_devices_table = nullptr;
    catalog::Label *m_devices_empty = nullptr;
    catalog::Section *m_routes_section = nullptr;
    catalog::Table *m_routes_table = nullptr;
    catalog::ComboBox *m_route_picker = nullptr;
    bool m_adding_route = false;
    catalog::Section *m_xpubs_section = nullptr;
    catalog::Table *m_xpubs_table = nullptr;
    catalog::Section *m_accounts_section = nullptr;
    catalog::Table *m_accounts_table = nullptr;
    catalog::Label *m_accounts_empty = nullptr;
    catalog::Notice *m_register_error = nullptr;
    catalog::Button *m_reveal = nullptr;
    catalog::Button *m_remove = nullptr;
    catalog::ModalActions *m_detail_actions = nullptr;
    catalog::ScrollArea *m_detail = nullptr;
    QWidget *m_header = nullptr;

    // Keyed by descriptor; cleared whenever the device changes.
    QHash<QString, Registration> m_registrations;
    QStringList m_register_queue;
    // The device the states above were read from, empty when none is ready.
    QString m_register_instance;
    QPointer<ISigner> m_register_signer;
    QString m_register_descriptor;
    ReqId m_register_req = 0;

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

    catalog::Label *m_remove_question = nullptr;
    catalog::Notice *m_remove_accounts = nullptr;
    catalog::Notice *m_remove_hot = nullptr;
    catalog::Notice *m_remove_error = nullptr;
    catalog::Button *m_remove_confirm = nullptr;
    catalog::ModalActions *m_remove_actions = nullptr;
    QWidget *m_remove_body = nullptr;
};

} // namespace keys::modal
