#pragma once

#include <QString>
#include <QWidget>

namespace catalog {

class Button;
class Label;

// − ●●●○○ + level control, under an optional label.
class Stepper : public QWidget {
    Q_OBJECT

public:
    explicit Stepper(QWidget *parent = nullptr);
    // Empty hides it.
    void setLabel(const QString &label);
    void setRange(int minimum, int maximum);
    void setValue(int value);
    [[nodiscard]] auto value() const -> int;

signals:
    void valueChanged(int value);

public slots:
    void onMinusClicked();
    void onPlusClicked();

protected:
    void updateState();

private:
    Label *m_label = nullptr;
    Button *m_minus = nullptr;
    Button *m_plus = nullptr;
    Label *m_value = nullptr;
    int m_minimum = 0;
    int m_maximum = 100;
    int m_current = 0;
};

} // namespace catalog
