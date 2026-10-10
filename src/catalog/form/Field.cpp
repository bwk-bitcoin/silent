#include "catalog/form/Field.h"

#include "catalog/display/Label.h"

#include <Qontrol>

namespace catalog {

Field::Field(const QString &label, QWidget *control, int label_width, QWidget *parent)
    : QWidget(parent),
      m_control(control) {
    m_label = new Label(label, LabelRole::Body, this);
    if (label_width != width::NONE) {
        m_label->setFixedWidth(label_width);
    }
    m_row = (new qontrol::Row)->spacing(resolve(Spacing::XS))->push(m_label)->push(m_control, 1);
    m_row->into(this);
}

auto Field::control() const -> QWidget * {
    return m_control;
}

void Field::setTopAligned(bool top) {
    Qt::Alignment alignment = top ? Qt::AlignTop : Qt::AlignVCenter;
    m_row->align(m_label, alignment);
    m_row->align(m_control, alignment);
}

} // namespace catalog
