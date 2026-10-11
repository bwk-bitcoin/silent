#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include "catalog/containers/Modal.h"
#include "theme/Palette.h"

namespace catalog {
class SelectRow;
class ViewSlots;
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
    void onFlowViewChanged(const QString &title);
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

// One way to add a key, embedded in AddKey once its mode is picked: its views, their
// button bars and the work behind them. Deleting it stops that work.
class AddKeyFlow : public QWidget {
    Q_OBJECT

public:
    explicit AddKeyFlow(QWidget *parent = nullptr);
    // Shows the first view, once AddKey listens to the signals.
    virtual void start() = 0;

signals:
    // The window title of the view now shown; every view keeps AddKey's one width.
    void viewChanged(const QString &title);
    // Back left the flow: AddKey shows its mode list again.
    void back();
    // The key is added: AddKey closes.
    void added();

protected:
    [[nodiscard]] auto viewSlots() const -> catalog::ViewSlots *;

private:
    catalog::ViewSlots *m_slots = nullptr;
};

} // namespace keys::modal
