#pragma once

#include <QString>

class QWidget;

namespace catalog {

class Button;
class FieldNote;
class Input;
class ValidationMark;

// An endpoint input, Test button and mark, with an error note below.
class EndpointRow {
public:
    enum class State { Untested, Testing, Ok, Failed };

    // Optional independent controls, placed after the test mark.
    explicit EndpointRow(QWidget *right_controls = nullptr);
    [[nodiscard]] auto control() const -> QWidget *;
    [[nodiscard]] auto note() const -> FieldNote *;
    [[nodiscard]] auto input() const -> Input *;
    [[nodiscard]] auto testButton() const -> Button *;
    [[nodiscard]] auto state() const -> State;
    // `error` is shown under the field in the Failed state.
    void setState(State state, const QString &error = QString());
    // Show an operation error without changing the test result; empty restores its note.
    void setError(const QString &error);
    // Gate the field and Test separately, e.g. Test only when a saved tuple differs.
    void setAvailable(bool available, bool test_available = true);

protected:
    void updateTestAvailability();

private:
    State m_state = State::Untested;
    QString m_error;
    bool m_available = true;
    bool m_test_available = true;
    Input *m_input = nullptr;
    Button *m_test = nullptr;
    Button *m_testing = nullptr;
    ValidationMark *m_mark = nullptr;
    FieldNote *m_note = nullptr;
    QWidget *m_control = nullptr;
};

} // namespace catalog
