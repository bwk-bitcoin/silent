#pragma once

#include <QHash>
#include <QString>
#include <QWidget>
#include <interfaces/keys.h>

class MusigAccountController;

namespace catalog {
class Button;
class CopyButton;
class Label;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig {

// What a shared wallet's Settings shows below its Electrum server: its policy, with
// Register Descriptor under it, and its descriptor, each key named by its alias.
class SettingsExtras : public QWidget {
    Q_OBJECT

public:
    explicit SettingsExtras(MusigAccountController *ctrl);

public slots:
    void onRegisterClicked();

protected:
    void init();
    void doConnect();
    void view();
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
};

} // namespace musig
