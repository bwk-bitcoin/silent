#pragma once

#include <optional>

#include <QPointer>
#include <QString>
#include <QStringList>

#include "catalog/containers/StepModal.h"

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
class ModalStatus;
class Notice;
class Section;
class Table;
class WordGrid;
} // namespace catalog

namespace keys {
class KeysModule;

namespace flow {
class PasswordStep;
} // namespace flow
} // namespace keys

namespace keys::modal {

class FetchXpubStep;

// One registered key: its alias, the devices it was seen on, its routes in priority
// order, its cached xpubs and the accounts using it. Fetching an xpub, revealing the
// mnemonic of a hot key and removing the key are steps of this same modal.
class KeyDetail final : public catalog::StepModal {
    Q_OBJECT

public:
    KeyDetail(KeysModule *module, const QString &fingerprint);
    ~KeyDetail() override;

public slots:
    void refresh();
    void refit();
    void onAliasEdited();
    void saveAlias();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onRemoveRouteClicked();
    void startAddingRoute();
    void confirmAddRoute();
    void cancelAddingRoute();
    void showDetail();
    void showFetch();
    void onFetchClicked(const QString &path);
    void onDeviceXpub(ReqId req_id, const QString &xpub);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message);
    void onInstanceRemoved(const QString &id);
    void showPassword();
    void onPasswordEntered(QString password);
    void onMnemonicRevealed(ReqId req_id, const MnemonicResult &result);
    void toggleWords();
    void closeWords();
    void showRemove();
    void removeKey();

protected:
    void init();
    void doConnect();
    void view();
    [[nodiscard]] auto info() const -> std::optional<KeyInfo>;
    [[nodiscard]] auto buildDetail() -> QWidget *;
    [[nodiscard]] auto buildReveal() -> QWidget *;
    [[nodiscard]] auto buildRemove() -> QWidget *;
    void refreshHeader(const KeyInfo &info);
    void refreshDevices();
    void refreshRoutes(const KeyInfo &info);
    void refreshXpubs();
    void refreshAccounts();
    void moveRoute(int from_index, int to_index);
    void showRouteResult(const KeyOpResult &result);
    // Every account whose descriptor this key signs for, by name.
    [[nodiscard]] auto accountNames() const -> QStringList;
    void stopFetch();

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
    catalog::Button *m_reveal = nullptr;
    catalog::ModalActions *m_detail_actions = nullptr;

    FetchXpubStep *m_fetch = nullptr;
    catalog::ModalStatus *m_fetching = nullptr;
    QPointer<ISigner> m_fetch_signer;
    QString m_fetch_instance;
    QString m_fetch_path;
    ReqId m_fetch_req = 0;

    flow::PasswordStep *m_password = nullptr;
    // The outstanding reveal request, so a stale answer is ignored.
    ReqId m_reveal_req = 0;
    catalog::WordGrid *m_words = nullptr;
    catalog::Button *m_hide_words = nullptr;
    catalog::Button *m_show_words = nullptr;
    catalog::ModalActions *m_reveal_actions = nullptr;

    catalog::Label *m_remove_question = nullptr;
    catalog::Notice *m_remove_accounts = nullptr;
    catalog::Notice *m_remove_hot = nullptr;
    catalog::Notice *m_remove_error = nullptr;
    catalog::ModalActions *m_remove_actions = nullptr;

    int m_detail_page = 0;
    int m_fetch_page = 0;
    int m_fetching_page = 0;
    int m_password_page = 0;
    int m_reveal_page = 0;
    int m_remove_page = 0;
};

} // namespace keys::modal
