#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <interfaces/backend/types.h>
#include <interfaces/keys.h>

class MusigAccountController;

namespace backend {
class IBlockchain;
} // namespace backend

namespace catalog {
class Button;
class ComboBox;
class CopyButton;
class Label;
class ValidatedInput;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig {

// What a shared wallet's Settings shows below its Electrum server: its blockchain backend,
// with a rescan from a height while a plugin supplies it, its policy, with Register
// Descriptor under it, and its descriptor, each key named by its alias.
class SettingsExtras : public QWidget {
    Q_OBJECT

public:
    explicit SettingsExtras(MusigAccountController *ctrl);

public slots:
    void onRegisterClicked();
    void onBackendActivated(int index);
    // The choice was refused or not saved: the saved one shows again, with `message`.
    void onBackendFailed(const QString &message);
    // Rescan is offered for a height entered, while no rescan is asked.
    void onRescanHeightChanged(const QString &text);
    // Asks the backend plugin to scan again from the height entered.
    void onRescanClicked();
    void onRescanCompleted(backend::RequestId request_id);
    void onRescanError(backend::RequestId request_id, const QString &message);

protected:
    void init();
    void doConnect();
    void view();
    // Electrum, then each installed blockchain backend, then a chosen one none supplies.
    void initBackend();
    // Shows the saved choice.
    void showSavedBackend();
    // The height to rescan from, shown only while the chosen plugin supplies the blockchain.
    void initRescan();
    // The alias of each key the keys module knows on the account's network.
    [[nodiscard]] static auto keyAliases(const QList<KeyEntry> &entries) -> QHash<QString, QString>;

private:
    MusigAccountController *m_controller = nullptr;
    qontrol::Column *m_policy = nullptr;
    catalog::Label *m_descriptor = nullptr;
    // Copies the descriptor as saved, not the shown one.
    catalog::CopyButton *m_copy_btn = nullptr;
    // Opens Register Descriptor: the wallet's devices to register the account on.
    catalog::Button *m_register_btn = nullptr;
    catalog::Label *m_backend_text = nullptr;
    // Each entry's data is its module id, empty for Electrum.
    catalog::ComboBox *m_backend_picker = nullptr;
    // A missing backend, a refusal or a failed save.
    catalog::Label *m_backend_error = nullptr;
    // The chosen plugin's blockchain, null for Electrum or a missing plugin.
    QPointer<backend::IBlockchain> m_backend;
    qontrol::Column *m_rescan = nullptr;
    catalog::ValidatedInput *m_rescan_input = nullptr;
    catalog::Button *m_rescan_btn = nullptr;
    // The rescan started, or the backend's refusal.
    catalog::Label *m_rescan_status = nullptr;
    // The rescan asked, 0 for none.
    backend::RequestId m_rescan_req = 0;
    quint64 m_rescan_height = 0;
};

} // namespace musig
