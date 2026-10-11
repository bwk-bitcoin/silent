#pragma once

#include <QWidget>

namespace qontrol {
class Row;
} // namespace qontrol

namespace catalog {

// Three slots stacked top to bottom: a header, a body centred across the width, and
// a footer. The consumer owns the widgets it puts in them and swaps them to move
// between its views.
class ViewSlots : public QWidget {
    Q_OBJECT

public:
    explicit ViewSlots(QWidget *parent = nullptr);
    // Each setter replaces its slot (null empties it) and refits the window. The
    // widget it replaces is hidden, not deleted.
    void setHeader(QWidget *header);
    void setBody(QWidget *body);
    void setFooter(QWidget *footer);
    // Puts `body` in the body filling the width, as the design's block bodies, instead
    // of centred at its own width.
    void setWideBody(QWidget *body);
    // Empties the header and the footer and puts `page` in the body, filling the width.
    void setPage(QWidget *page);
    // Hides `widget` and keeps it here until a view shows it.
    void keep(QWidget *widget);

protected:
    void replace(qontrol::Row *slot, QWidget *&current, QWidget *widget, bool centred);

private:
    qontrol::Row *m_header = nullptr;
    qontrol::Row *m_body = nullptr;
    qontrol::Row *m_footer = nullptr;
    QWidget *m_header_widget = nullptr;
    QWidget *m_body_widget = nullptr;
    QWidget *m_footer_widget = nullptr;
};

} // namespace catalog
