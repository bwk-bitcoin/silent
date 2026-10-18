#pragma once

#include "theme/Palette.h"
#include <QString>
#include <Qontrol>

class QDialog;
class QEvent;

namespace catalog {

class ViewSlots;

// A modal of three slots stacked top to bottom: a header, a body centred across the
// window, and a footer. The consumer owns the widgets it puts in them and swaps them
// to move between its views.
class Modal : public qontrol::Modal {
    Q_OBJECT

public:
    Modal();
    // Each setter replaces its slot (null empties it) and refits the window. The
    // widget it replaces is hidden, not deleted.
    void setHeader(QWidget *header);
    void setBody(QWidget *body);
    // Spanning the body's width, not centred at its own.
    void setWideBody(QWidget *body);
    void setFooter(QWidget *footer);
    // Empties the header and the footer and puts `page` in the body, filling the width.
    void setPage(QWidget *page);
    void setWidth(ModalWidth width);
    // A wizard keeps one height on every step (metric::MODAL_WIZARD_HEIGHT), its body
    // filling it; taller content grows it.
    void setWizardHeight(bool wizard);
    // Opens over the active window, centred on it at the size its content lays out to.
    void setVisible(bool visible) override;

protected:
    // Hides `widget` and gives it to the modal, kept until a view shows it.
    void keep(QWidget *widget);
    // Fits the window to its content at its fixed width.
    void refit();
    // Lays out every layout of the content, deepest first. A layout lays out on its
    // LayoutRequest only once its widget is visible, so content set while the modal
    // is hidden (a body swapped before exec()) is measured at its old size without it.
    void activateLayouts();
    auto event(QEvent *event) -> bool override;

private:
    ViewSlots *m_slots = nullptr;
    bool m_wizard_height = false;
};

// Keeps only Close in the window manager titlebar of `dialog`: a modal is neither
// minimized nor maximized.
void closeOnlyTitlebar(QDialog *dialog);
// For a dialog that is not a catalog::Modal: Close only, and sized to its content so
// it cannot be resized, as KWin keeps maximize on a resizable window whatever the hints.
void fixedDialog(QDialog *dialog);

// The title on the left of a modal header, with an optional widget on its right (a
// fingerprint and a status pill).
auto modalHeader(const QString &title, QWidget *end = nullptr) -> QWidget *;

} // namespace catalog
