#include "catalog/form/EndpointRow.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/form/FieldNote.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/Input.h"
#include "theme/Palette.h"

#include <Qontrol>

namespace catalog {

EndpointRow::EndpointRow() {
    m_input = new Input(InputRole::Mono);
    m_test = btnTest();
    m_testing = btnTesting();
    m_testing->setEnabled(false);
    m_testing->setVisible(false);
    m_mark = new ValidationMark;
    m_note = new FieldNote(QString(), FieldNote::Tone::Error);
    m_note->setVisible(false);
    m_control = (new qontrol::Row)
                    ->spacing(resolve(Spacing::XS))
                    ->push(m_input, 1)
                    ->push(m_test)
                    ->push(m_testing)
                    ->push(m_mark);
}

auto EndpointRow::control() const -> QWidget * {
    return m_control;
}

auto EndpointRow::note() const -> FieldNote * {
    return m_note;
}

auto EndpointRow::input() const -> Input * {
    return m_input;
}

auto EndpointRow::testButton() const -> Button * {
    return m_test;
}

auto EndpointRow::state() const -> State {
    return m_state;
}

void EndpointRow::setState(State state, const QString &error) {
    m_state = state;
    bool testing = state == State::Testing;
    m_test->setVisible(!testing);
    m_testing->setVisible(testing);
    m_test->setEnabled(!m_input->text().trimmed().isEmpty());
    switch (state) {
    case State::Ok:
        m_mark->setState(ValidationMark::State::Valid);
        break;
    case State::Failed:
        m_mark->setState(ValidationMark::State::Invalid);
        break;
    case State::Untested:
    case State::Testing:
        m_mark->setState(ValidationMark::State::None);
        break;
    }
    m_note->setText(error);
    m_note->setVisible(state == State::Failed && !error.isEmpty());
}

} // namespace catalog
