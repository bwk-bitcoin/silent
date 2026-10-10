#include "catalog/form/ValidatedInput.h"

#include "theme/Palette.h"

#include <QResizeEvent>
#include <Qontrol>
#include <common.h>

namespace catalog {

ValidatedInput::ValidatedInput(InputRole role, QWidget *parent) : QWidget(parent) {
    m_input = new Input(role, this);
    // Mark is a child of the input so it overlays inside the field's right padding.
    m_mark = new ValidationMark(m_input);

    (new qontrol::Row)->push(m_input)->into(this);

    // Reserve room at the input's right edge so text never runs under the mark.
    m_input->setTextMargins(0, 0, metric::VALIDATION_MARK_WIDTH + resolve(Spacing::XS), 0);

    connect(m_input, &Input::textChanged, this, &ValidatedInput::textChanged, qontrol::UNIQUE);
}

void ValidatedInput::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    int markWidth = metric::VALIDATION_MARK_WIDTH;
    // The input fills this widget, and is not resized yet when this one is.
    int markX = width() - markWidth - resolve(Spacing::XS);
    m_mark->setGeometry(markX, 0, markWidth, height());
}

auto ValidatedInput::input() const -> Input * {
    return m_input;
}

void ValidatedInput::setWidth(Size s) {
    m_input->setWidth(s);
}

void ValidatedInput::setState(ValidationMark::State state) {
    m_mark->setState(state);
}

void ValidatedInput::setValid(bool valid) {
    m_mark->setState(valid ? ValidationMark::State::Valid : ValidationMark::State::Invalid);
}

} // namespace catalog
