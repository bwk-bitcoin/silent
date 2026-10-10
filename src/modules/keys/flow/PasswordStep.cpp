#include "PasswordStep.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QLineEdit>
#include <Qontrol>
#include <common.h>

namespace keys::flow {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

PasswordStep::PasswordStep(catalog::Button *proceed, QWidget *parent) : QWidget(parent) {
    init(proceed);
    doConnect();
    view();
    updateActions();
}

PasswordStep::~PasswordStep() {
    m_input->clear();
}

void PasswordStep::init(catalog::Button *proceed) {
    m_prompt = new Label(LabelRole::Body);
    m_prompt->setTone(LabelTone::Secondary);
    m_prompt->setWordWrap(true);
    m_prompt->setVisible(false);

    m_input = new catalog::Input;
    m_input->setEchoMode(QLineEdit::Password);
    m_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);

    m_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_error->setVisible(false);

    m_proceed = proceed;
    m_unlocking = catalog::btnUnlocking();
    m_unlocking->setEnabled(false);
    m_unlocking->setVisible(false);
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_unlocking);
    m_actions->addAction(m_proceed);
}

void PasswordStep::doConnect() {
    connect(m_input, &QLineEdit::textEdited, this, &PasswordStep::onTextEdited, qontrol::UNIQUE);
    connect(m_input, &QLineEdit::returnPressed, this, &PasswordStep::onProceedClicked,
            qontrol::UNIQUE);
    connect(m_proceed, &QPushButton::clicked, this, &PasswordStep::onProceedClicked,
            qontrol::UNIQUE);
    connect(m_actions, &catalog::ModalActions::backClicked, this, &PasswordStep::backClicked,
            qontrol::UNIQUE);
}

void PasswordStep::view() {
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_prompt)
        ->push(new catalog::Field(TR("flow-password"), m_input))
        ->push(m_error)
        ->into(this);
}

void PasswordStep::setPrompt(const QString &prompt) {
    m_prompt->setText(prompt);
    m_prompt->setVisible(!prompt.isEmpty());
}

void PasswordStep::setBackVisible(bool visible) {
    m_actions->setBack(visible ? catalog::ModalActions::Back::Back
                               : catalog::ModalActions::Back::None);
}

void PasswordStep::showError(const QString &error) {
    m_error->setText(error);
    m_error->setVisible(true);
}

void PasswordStep::setBusy(bool busy) {
    m_busy = busy;
    m_input->setEnabled(!busy);
    updateActions();
}

void PasswordStep::reset() {
    m_input->clear();
    m_error->setVisible(false);
    setBusy(false);
    m_input->setFocus();
}

auto PasswordStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void PasswordStep::onTextEdited() {
    m_error->setVisible(false);
    updateActions();
}

void PasswordStep::onProceedClicked() {
    if (m_busy || m_input->text().isEmpty()) {
        return;
    }
    QString password = m_input->text();
    m_input->clear();
    m_error->setVisible(false);
    emit passwordEntered(password);
    password.fill(QChar());
}

void PasswordStep::updateActions() {
    bool empty = m_input->text().isEmpty();
    m_proceed->setVisible(!m_busy);
    m_proceed->setEnabled(!empty);
    m_unlocking->setVisible(m_busy);
    m_actions->setReason(empty && !m_busy ? TR("flow-password-reason") : QString());
}

} // namespace keys::flow
