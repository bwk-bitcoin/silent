#pragma once

#include <QString>
#include <QWidget>

namespace qontrol {
class Row;
}

namespace catalog {

class Label;

// A label right before its control, as wide as its text; stacked fields that must
// line up go in a FieldGroup.
class Field : public QWidget {
    Q_OBJECT

public:
    Field(const QString &label, QWidget *control, QWidget *parent = nullptr);
    [[nodiscard]] auto control() const -> QWidget *;
    // Label and control sit at the top instead of the middle (multi-line controls).
    void setTopAligned(bool top);

private:
    Label *m_label = nullptr;
    QWidget *m_control = nullptr;
    qontrol::Row *m_row = nullptr;
};

} // namespace catalog
