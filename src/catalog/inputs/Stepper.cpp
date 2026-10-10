#include "catalog/inputs/Stepper.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace catalog {

Stepper::Stepper(QWidget *parent) : QWidget(parent) {
    m_minus = btnStepperMinus();
    m_value = new Label("0", LabelRole::Mono, this);
    m_value->setAlignment(Qt::AlignCenter);
    m_value->setFixedWidth(resolve(Size::XXS));
    m_plus = btnStepperPlus();

    (new qontrol::Row)
        ->spacing(resolve(Spacing::XS))
        ->push(m_minus)
        ->push(m_value)
        ->push(m_plus)
        ->into(this);

    connect(m_minus, &Button::clicked, this, &Stepper::onMinusClicked, qontrol::UNIQUE);
    connect(m_plus, &Button::clicked, this, &Stepper::onPlusClicked, qontrol::UNIQUE);
    updateState();
}

void Stepper::setRange(int minimum, int maximum) {
    m_minimum = minimum;
    m_maximum = maximum;
    setValue(m_current);
}

void Stepper::setValue(int value) {
    int clamped = qBound(m_minimum, value, m_maximum);
    if (m_current == clamped) {
        updateState();
        return;
    }
    m_current = clamped;
    updateState();
    emit valueChanged(m_current);
}

auto Stepper::value() const -> int {
    return m_current;
}

void Stepper::onMinusClicked() {
    setValue(m_current - 1);
}

void Stepper::onPlusClicked() {
    setValue(m_current + 1);
}

void Stepper::updateState() {
    m_value->setText(QString::number(m_current));
    m_minus->setEnabled(m_current > m_minimum);
    m_plus->setEnabled(m_current < m_maximum);
}

} // namespace catalog
