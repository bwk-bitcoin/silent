#pragma once

#include <QString>

#include <Qontrol>
#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Button;
class ComboBox;
class Input;
class Label;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

// Confirms removing a key that still has registered descriptors. The wording is
// fixed: there is nothing key-specific to show beyond what removal drops.
class RemoveKeyConfirm final : public qontrol::Modal {
    Q_OBJECT

public:
    explicit RemoveKeyConfirm(QWidget *parent = nullptr);
    ~RemoveKeyConfirm() override = default;

signals:
    void confirmed();

public slots:
    void onRemoveClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    catalog::Label *m_label = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    catalog::Button *m_remove_btn = nullptr;
};

// One registered key: its alias, its routes in priority order, the devices it has been
// seen on, its cached xpubs and the descriptors registered with it. Every edit goes
// straight to the key record and the affected list is rebuilt in place.
class KeyModal final : public qontrol::Modal {
    Q_OBJECT

public:
    explicit KeyModal(KeysModule *module, QString fingerprint, QWidget *parent = nullptr);
    ~KeyModal() override;

public slots:
    void onSaveAliasClicked();
    void onAddRouteClicked();
    void onRemoveRouteClicked();
    void onMoveRouteUpClicked();
    void onMoveRouteDownClicked();
    void onRemoveKeyClicked();
    void onRemoveKeyConfirmed();
    void onRevealClicked();
    void onRevealPasswordEntered(QString password);
    void onMnemonicRevealed(ReqId req_id, MnemonicResult result);

protected:
    void init();
    void doConnect();
    void view();

    void rebuildRoutes();
    void rebuildDevices();
    void rebuildXpubs();
    void rebuildDescriptors();
    void updateRemoveState();
    void moveRoute(int from_index, int to_index);
    void showError(const QString &message);
    void clearRevealedMnemonic();
    [[nodiscard]] auto routeAt(int index) const -> KeyRouteKind;

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    QString m_revealed_mnemonic;
    // The id of the outstanding revealMnemonic() request, so a stale answer (from a
    // request this modal already moved past) is ignored.
    ReqId m_reveal_req_id = 0;
    catalog::Input *m_alias_input = nullptr;
    catalog::Button *m_save_alias_btn = nullptr;
    catalog::ComboBox *m_route_picker = nullptr;
    catalog::Button *m_add_route_btn = nullptr;
    catalog::Button *m_reveal_btn = nullptr;
    catalog::Label *m_revealed_mnemonic_label = nullptr;
    catalog::Button *m_remove_key_btn = nullptr;
    catalog::Button *m_close_btn = nullptr;
    catalog::Label *m_remove_hint = nullptr;
    catalog::Label *m_error_label = nullptr;
    catalog::Label *m_devices_empty = nullptr;
    catalog::Label *m_xpubs_empty = nullptr;
    catalog::Label *m_descriptors_empty = nullptr;
    qontrol::Column *m_route_column = nullptr;
    qontrol::Column *m_device_column = nullptr;
    qontrol::Column *m_xpub_column = nullptr;
    qontrol::Column *m_descriptor_column = nullptr;
};

} // namespace keys::modal
