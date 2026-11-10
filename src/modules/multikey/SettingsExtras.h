#pragma once

#include <QHash>
#include <QString>
#include <QWidget>
#include <interfaces/keys.h>

class MusigAccountController;

namespace catalog {
class Button;
class ComboBox;
class CopyButton;
class Label;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig {

// What a shared wallet's Settings shows below its Electrum server: its blockchain backend,
// its policy, with Register Descriptor under it, and its descriptor, each key named by its
// alias.
class SettingsExtras : public QWidget {
    Q_OBJECT

public:
    explicit SettingsExtras(MusigAccountController *ctrl);

public slots:
    void onRegisterClicked();
    void onBackendActivated(int index);
    // The choice was refused or not saved: the saved one shows again, with `message`.
    void onBackendFailed(const QString &message);

protected:
    void init();
    void doConnect();
    void view();
    // Electrum, then each installed blockchain backend, then a chosen one none supplies.
    void initBackend();
    // Shows the saved choice.
    void showSavedBackend();
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
};

} // namespace musig
