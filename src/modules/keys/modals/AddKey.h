#pragma once

#include <QList>
#include <QString>

#include "catalog/containers/Modal.h"

namespace catalog {
class SelectRow;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

class AddKeyFlow;

// Adds a key no connected device registers: an airgap key over QR codes, a key file,
// a pasted xpub with its origin, or a hot key generated or imported here and sealed
// with a password. The mode list, then the flow of the mode picked in its place.
class AddKey final : public catalog::Modal {
    Q_OBJECT

public:
    explicit AddKey(KeysModule *module);
    void done(int result) override;

public slots:
    void onModeClicked();
    void onFlowViewChanged(const QString &title, ModalWidth width);
    void onFlowBack();

protected:
    void init();
    void doConnect();
    void view();

    // Modes view
    void initModes();
    void connectModes();
    [[nodiscard]] auto viewModes() -> QWidget *;
    void showModes();

    // Shared
    void closeFlow();

private:
    KeysModule *m_module = nullptr;
    QList<catalog::SelectRow *> m_mode_rows;
    QWidget *m_modes_body = nullptr;
    // The flow of the mode picked, deleted once it goes back or the window closes.
    AddKeyFlow *m_flow = nullptr;
};

} // namespace keys::modal
