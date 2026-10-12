#include "Unseal.h"

#include "catalog/Button.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QLineEdit>
#include <QTimer>
#include <Qontrol>

#include <common.h>

namespace keys::modal {

namespace {

// A hot key answers at once: the spinner shows at least this long, not as a flash.
constexpr int BUSY_MIN_MS = 500;

} // namespace

Unseal::Unseal(const QString &title, const QString &prompt, catalog::Button *proceed)
    : m_title(title),
      m_prompt_text(prompt),
      m_proceed(proceed) {
    init();
    doConnect();
    view();
}

Unseal::~Unseal() {
    m_password_input->clear();
}

void Unseal::setBusy(const QString &title, const QString &subtitle) {
    m_busy_step->setTitle(title);
    m_busy_step->setSubtitle(subtitle);
}

void Unseal::setBackShown(bool shown) {
    m_password_actions->setBack(shown ? catalog::ModalActions::Back::Back
                                      : catalog::ModalActions::Back::None);
}

void Unseal::finish(bool accepted) {
    hold(accepted ? Outcome::Accept : Outcome::Reject);
}

void Unseal::retry(const QString &error) {
    m_retry_error = error;
    hold(Outcome::Retry);
}

void Unseal::init() {
    initPassword();
    initBusy();
}

void Unseal::doConnect() {
    connect(m_password_input, &QLineEdit::textEdited, this, &Unseal::onPasswordEdited,
            qontrol::UNIQUE);
    connect(m_password_input, &QLineEdit::returnPressed, this, &Unseal::onNextClicked,
            qontrol::UNIQUE);
    connect(m_proceed, &QPushButton::clicked, this, &Unseal::onNextClicked, qontrol::UNIQUE);
    connect(m_password_actions, &catalog::ModalActions::backClicked, this, &QDialog::reject,
            qontrol::UNIQUE);
    connect(m_busy_hold, &QTimer::timeout, this, &Unseal::onBusyHeld, qontrol::UNIQUE);
}

void Unseal::view() {
    m_password_step = viewPassword();
    hideBusy();
    showPassword();
}

// ===== Password view =====

void Unseal::initPassword() {
    m_prompt = new catalog::Label(catalog::LabelRole::Body);
    m_prompt->setTone(catalog::LabelTone::Secondary);
    m_prompt->setWordWrap(true);
    m_prompt->setText(m_prompt_text);
    m_prompt->setVisible(!m_prompt_text.isEmpty());
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_password_actions = new catalog::ModalActions;
    m_password_actions->setBack(catalog::ModalActions::Back::Back);
    m_password_actions->addAction(m_proceed);
}

auto Unseal::viewPassword() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_prompt)
        ->push(new catalog::Field(TR("flow-password"), m_password_input))
        ->push(m_password_error);
}

// An empty field with the focus in it, and the error kept if one was just shown.
void Unseal::showPassword() {
    m_view = View::Password;
    m_password_input->clear();
    onPasswordEdited();
    setWindowTitle(m_title);
    setHeader(nullptr);
    setBody(m_password_step);
    setFooter(m_password_actions);
    setWidth(ModalWidth::S);
    m_password_input->setFocus();
}

void Unseal::proceedPassword() {
    if (m_password_input->text().isEmpty()) {
        return;
    }
    QString password = m_password_input->text();
    m_password_input->clear();
    m_password_error->setVisible(false);
    showBusy();
    emit passwordEntered(password);
    password.fill(QChar());
    password.clear();
}

// ===== Busy view =====

void Unseal::initBusy() {
    m_busy_step = new catalog::ModalStatus;
    m_busy_step->setSpinner();
    m_busy_step->setPadding(resolve(Spacing::M), resolve(Spacing::M));
    m_busy_hold = new QTimer(this);
    m_busy_hold->setSingleShot(true);
}

void Unseal::showBusy() {
    m_view = View::Busy;
    m_outcome = Outcome::None;
    setHeader(nullptr);
    setBody(m_busy_step);
    setFooter(nullptr);
    setWidth(ModalWidth::S);
    m_busy_shown.start();
}

void Unseal::hideBusy() {
    keep(m_busy_step);
}

void Unseal::hold(Outcome outcome) {
    m_outcome = outcome;
    qint64 remaining = BUSY_MIN_MS - m_busy_shown.elapsed();
    if (m_view != View::Busy || remaining <= 0) {
        apply();
        return;
    }
    m_busy_hold->start(static_cast<int>(remaining));
}

void Unseal::apply() {
    Outcome outcome = m_outcome;
    m_outcome = Outcome::None;
    switch (outcome) {
    case Outcome::Accept:
        accept();
        return;
    case Outcome::Reject:
        reject();
        return;
    case Outcome::Retry:
        m_password_error->setText(m_retry_error);
        m_password_error->setVisible(true);
        showPassword();
        return;
    case Outcome::None:
        return;
    }
}

// ===== Slots =====

void Unseal::onNextClicked() {
    switch (m_view) {
    case View::Password:
        proceedPassword();
        return;
    case View::Busy:
        return;
    }
}

void Unseal::onPasswordEdited() {
    bool empty = m_password_input->text().isEmpty();
    if (!empty) {
        m_password_error->setVisible(false);
    }
    m_proceed->setEnabled(!empty);
    m_password_actions->setReason(empty ? TR("flow-password-reason") : QString());
}

void Unseal::onBusyHeld() {
    apply();
}

} // namespace keys::modal
