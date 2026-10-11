#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "catalog/containers/Modal.h"
#include "theme/Palette.h"

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
class ViewSlots;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

class KeyDetailFlow;
struct KeyAccount;

// Where an account's descriptor stands on the key's connected device.
enum class Registration { Unknown, Checking, Registering, Registered, NotRegistered };

// One registered key: its alias, the devices it was seen on, its routes in priority
// order, its cached xpubs and the accounts using it, with each account's descriptor
// checked and registered on the key's connected device. Revealing the mnemonic of
// a hot key and removing the key are flows embedded in its place; fetching an xpub
// opens its own modal over it.
class KeyDetail final : public catalog::Modal {
    Q_OBJECT

public:
    KeyDetail(KeysModule *module, const QString &fingerprint);
    void done(int result) override;

public slots:
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
    void onRemoveXpubClicked();
    void onRegisterClicked();
    void onDescriptorIsRegistered(ReqId req_id, bool registered);
    void onDescriptorRegistered(ReqId req_id, bool registered);
    void onRegistrationError(std::optional<ReqId> req_id, const QString &message);
    void onRevealClicked();
    void onRemoveClicked();
    void onFlowViewChanged(const QString &title, ModalWidth width);
    void onFlowBack();

protected:
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
    [[nodiscard]] auto registrationCell(const KeyAccount &account, bool applies) -> QWidget *;
    // Follows the key's ready device: a new one is asked about every account, and
    // accounts added since are asked about on the same one.
    void syncRegistrations(const QList<KeyAccount> &accounts, bool applies);
    // Sends the next queued check or registration, one at a time on the device.
    void sendNextRegistration();
    void finishRegistrationRequest(Registration state);
    void stopRegistrations();
    void showRegistrationError(const QString &message);

    // Shared
    // The title on the left, the fingerprint and the key status on the right.
    void showHeader(const QString &title);
    void openFlow(KeyDetailFlow *flow);
    void closeFlow();

private:
    KeysModule *m_module = nullptr;
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

    // The flow picked off the detail, deleted once it goes back or the window closes.
    KeyDetailFlow *m_flow = nullptr;
};

// One step off a key's detail, embedded in KeyDetail in place of the detail: its views,
// their button bars and the work behind them, under the detail's header. Deleting it
// stops that work.
class KeyDetailFlow : public QWidget {
    Q_OBJECT

public:
    KeyDetailFlow(KeysModule *module, QString fingerprint);
    // Shows the first view, once KeyDetail listens to the signals.
    virtual void start() = 0;

signals:
    // The window title and width of the view now shown.
    void viewChanged(const QString &title, ModalWidth width);
    // Back left the flow: KeyDetail shows the detail again.
    void back();
    // The key is removed: KeyDetail closes.
    void removed();

public slots:
    void onKeyChanged();

protected:
    [[nodiscard]] auto viewSlots() const -> catalog::ViewSlots *;
    [[nodiscard]] auto module() const -> KeysModule *;
    [[nodiscard]] auto fingerprint() const -> const QString &;
    // The detail's header under `title`, kept up to date with the key.
    void showHeader(const QString &title);

private:
    catalog::ViewSlots *m_slots = nullptr;
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    QString m_title;
    QWidget *m_header = nullptr;
};

} // namespace keys::modal
