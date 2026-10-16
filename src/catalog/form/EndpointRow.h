#pragma once

#include <QString>

class QWidget;

namespace catalog {

class Button;
class FieldNote;
class Input;
class ValidationMark;

// A backend endpoint: a mono input, its Test button and mark, and under it the
// reason the last test failed.
class EndpointRow {
public:
    enum class State { Untested, Testing, Ok, Failed };

    EndpointRow();
    [[nodiscard]] auto control() const -> QWidget *;
    [[nodiscard]] auto note() const -> FieldNote *;
    [[nodiscard]] auto input() const -> Input *;
    [[nodiscard]] auto testButton() const -> Button *;
    [[nodiscard]] auto state() const -> State;
    // `error` is shown under the field in the Failed state.
    void setState(State state, const QString &error = QString());

private:
    State m_state = State::Untested;
    Input *m_input = nullptr;
    Button *m_test = nullptr;
    Button *m_testing = nullptr;
    ValidationMark *m_mark = nullptr;
    FieldNote *m_note = nullptr;
    QWidget *m_control = nullptr;
};

} // namespace catalog
