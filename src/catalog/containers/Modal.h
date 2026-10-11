#pragma once

#include "theme/Palette.h"
#include <QString>
#include <Qontrol>

class QEvent;

namespace catalog {

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
    void setFooter(QWidget *footer);
    void setWidth(ModalWidth width);

protected:
    // Hides `widget` and gives it to the modal, kept until a view shows it.
    void keep(QWidget *widget);
    auto event(QEvent *event) -> bool override;

private:
    void replace(qontrol::Row *slot, QWidget *&current, QWidget *widget);

    qontrol::Row *m_header = nullptr;
    qontrol::Row *m_body = nullptr;
    qontrol::Row *m_footer = nullptr;
    QWidget *m_header_widget = nullptr;
    QWidget *m_body_widget = nullptr;
    QWidget *m_footer_widget = nullptr;
};

// The title on the left of a modal header, with an optional widget on its right (a
// fingerprint and a status pill).
auto modalHeader(const QString &title, QWidget *end = nullptr) -> QWidget *;

} // namespace catalog
