#include "PasswordPrompt.h"

#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "catalog/form/FormRow.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "views/utils.h"

#include <QLineEdit>
#include <utility>

#include <common.h>

using catalog::Button;
using catalog::ButtonRole;
using catalog::FormRow;
using catalog::Input;
using catalog::Label;
using catalog::LabelRole;

namespace keys::modal {

namespace {

constexpr int MIN_PASSWORD_LENGTH = 8;

} // namespace

PasswordPrompt::PasswordPrompt(Mode mode, QString title, QWidget *parent)
    : m_mode(mode),
      m_title(std::move(title)) {
    Q_UNUSED(parent);
    setWindowTitle(m_title);
    init();
    doConnect();
    view();
}

PasswordPrompt::~PasswordPrompt() {
    clearPassword();
}

void PasswordPrompt::init() {
    m_password_input = new Input;
    m_password_input->setEchoMode(QLineEdit::Password);

    m_confirm_input = new Input;
    m_confirm_input->setEchoMode(QLineEdit::Password);
    m_confirm_row = new FormRow(TR("keys-password-confirm"), m_confirm_input);
    m_confirm_row->setVisible(m_mode == Mode::Create);

    m_error_label = new Label(QString(), LabelRole::Caption);
    m_error_label->setVisible(false);

    m_confirm_btn = new Button(TR("common-proceed"), ButtonRole::Primary);
    m_confirm_btn->setEnabled(false);
    m_cancel_btn = new Button(TR("common-cancel"));
}

void PasswordPrompt::doConnect() {
    connect(m_password_input, &QLineEdit::textChanged, this, &PasswordPrompt::onTextChanged,
            qontrol::UNIQUE);
    connect(m_confirm_input, &QLineEdit::textChanged, this, &PasswordPrompt::onTextChanged,
            qontrol::UNIQUE);
    connect(m_confirm_btn, &QPushButton::clicked, this, &PasswordPrompt::onConfirmClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &QDialog::reject, qontrol::UNIQUE);
}

void PasswordPrompt::view() {
    auto *buttonRow = (new qontrol::Row)
                          ->pushSpacer()
                          ->push(m_cancel_btn)
                          ->pushSpacer(resolve(Spacing::XS))
                          ->push(m_confirm_btn);

    auto *col = (new qontrol::Column)
                    ->push(new Label(m_title, LabelRole::Heading))
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(new FormRow(TR("keys-password"), m_password_input))
                    ->push(m_confirm_row)
                    ->push(m_error_label)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(buttonRow);

    setMainWidget(margin(col));
}

auto PasswordPrompt::password() const -> QString {
    return m_password;
}

void PasswordPrompt::clearPassword() {
    m_password.fill(QChar());
    m_password.clear();
    m_password_input->setText(QString());
    m_confirm_input->setText(QString());
}

void PasswordPrompt::onTextChanged() {
    QString password = m_password_input->text();
    bool longEnough = password.size() >= MIN_PASSWORD_LENGTH;

    if (m_mode == Mode::Use) {
        m_error_label->setVisible(false);
        m_confirm_btn->setEnabled(longEnough);
        return;
    }

    QString confirm = m_confirm_input->text();
    bool matches = password == confirm;

    if (!password.isEmpty() && !longEnough) {
        m_error_label->setText(TR("keys-password-short"));
        m_error_label->setVisible(true);
    } else if (!confirm.isEmpty() && !matches) {
        m_error_label->setText(TR("keys-password-mismatch"));
        m_error_label->setVisible(true);
    } else {
        m_error_label->setVisible(false);
    }

    m_confirm_btn->setEnabled(longEnough && matches);
}

void PasswordPrompt::onConfirmClicked() {
    m_password = m_password_input->text();
    emit passwordEntered(m_password);
    m_password_input->setText(QString());
    m_confirm_input->setText(QString());
    accept();
}

} // namespace keys::modal
