#pragma once

#include <QString>
#include <QWidget>

#include "theme/Palette.h"

namespace catalog {
class ViewSlots;
} // namespace catalog

namespace keys::modal {

// One way to add a key, embedded in AddKey once its mode is picked: its views, their
// button bars and the work behind them. Deleting it stops that work.
class AddKeyFlow : public QWidget {
    Q_OBJECT

public:
    explicit AddKeyFlow(QWidget *parent = nullptr);
    // Shows the first view, once AddKey listens to the signals.
    virtual void start() = 0;

signals:
    // The window title and width of the view now shown.
    void viewChanged(const QString &title, ModalWidth width);
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
