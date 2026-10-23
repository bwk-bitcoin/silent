#include "catalog/form/ValidatedInput.h"

#include "theme/Palette.h"

#include <QEvent>
#include <QStyle>
#include <Qontrol>
#include <common.h>

namespace catalog {

ValidatedInput::ValidatedInput(InputRole role, QWidget *parent) : QWidget(parent) {
    m_input = new Input(role, this);
    // Mark is a child of the input so it overlays inside the field's right padding.
    m_mark = new ValidationMark(m_input);
    m_mark->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_input->installEventFilter(this);

    (new qontrol::Row)->push(m_input)->into(this);

    // Reserve room at the input's right edge so text never runs under the mark.
    m_input->setTextMargins(0, 0, metric::VALIDATION_MARK_WIDTH + resolve(Spacing::XS), 0);

    connect(m_input, &Input::textChanged, this, &ValidatedInput::textChanged, qontrol::UNIQUE);
}

auto ValidatedInput::eventFilter(QObject *watched, QEvent *event) -> bool {
    if (watched == m_input && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        int markWidth = metric::VALIDATION_MARK_WIDTH;
        int markX = m_input->width() - markWidth - resolve(Spacing::XS);
        m_mark->setGeometry(markX, 0, markWidth, m_input->height());
    }
    return QWidget::eventFilter(watched, event);
}

auto ValidatedInput::input() const -> Input * {
    return m_input;
}

void ValidatedInput::setWidth(Size s) {
    m_input->setWidth(s);
}

void ValidatedInput::setState(ValidationMark::State state) {
    if (m_mark->state() == state) {
        return;
    }
    m_mark->setState(state);
    m_mark->style()->unpolish(m_mark);
    m_mark->style()->polish(m_mark);
    m_mark->update();
}

void ValidatedInput::setValid(bool valid) {
    setState(valid ? ValidationMark::State::Valid : ValidationMark::State::Invalid);
}

} // namespace catalog
