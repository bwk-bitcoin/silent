#include "catalog/inputs/Stepper.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace catalog {

namespace {

const QChar FILLED_DOT(0x25CF);
const QChar EMPTY_DOT(0x25CB);

} // namespace

Stepper::Stepper(QWidget *parent) : QWidget(parent) {
    m_label = new Label(LabelRole::Body, this);
    m_label->setTone(LabelTone::Secondary);
    auto labelFont = m_label->font();
    labelFont.setWeight(QFont::Medium); // design label is weight 500
    m_label->setFont(labelFont);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setVisible(false);
    m_minus = btnStepperMinus();
    m_value = new Label(LabelRole::Mono, this);
    m_value->setTone(LabelTone::Accent);
    m_value->setAlignment(Qt::AlignCenter);
    m_value->setMinimumWidth(metric::STEPPER_DOTS_WIDTH);
    m_plus = btnStepperPlus();

    auto *controls = (new qontrol::Row)
                         ->spacing(resolve(Spacing::S))
                         ->push(m_minus)
                         ->push(m_value)
                         ->push(m_plus);
    (new qontrol::Column)
        ->spacing(resolve(Spacing::XS))
        ->pushAligned(m_label, Qt::AlignHCenter)
        ->pushAligned(controls, Qt::AlignHCenter)
        ->into(this);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    connect(m_minus, &Button::clicked, this, &Stepper::onMinusClicked, qontrol::UNIQUE);
    connect(m_plus, &Button::clicked, this, &Stepper::onPlusClicked, qontrol::UNIQUE);
    updateState();
}

void Stepper::setLabel(const QString &label) {
    m_label->setText(label);
    m_label->setVisible(!label.isEmpty());
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
    int filled = m_current - m_minimum + 1;
    int empty = m_maximum - m_current;
    m_value->setText(QString(filled, FILLED_DOT) + QString(empty, EMPTY_DOT));
    m_minus->setEnabled(m_current > m_minimum);
    m_plus->setEnabled(m_current < m_maximum);
}

} // namespace catalog
