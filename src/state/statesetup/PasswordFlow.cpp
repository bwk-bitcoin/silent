#include "PasswordFlow.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QEvent>
#include <QLineEdit>
#include <QThread>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <string>
#include <utility>

namespace state {

namespace {
constexpr int MIN_PASSWORD_LENGTH = 8;
}

PasswordFlow::PasswordFlow() {
    init();
    doConnect();
    view();
}

PasswordFlow::~PasswordFlow() {
    clearPassword();
}

void PasswordFlow::start() {
    showPassword();
}

void PasswordFlow::init() {
    initPassword();
    initBusy();
}

void PasswordFlow::doConnect() {
    connectPassword();
}

void PasswordFlow::view() {
    m_password = viewPassword();
    m_busy = viewBusy();
    hidePassword();
    hideBusy();
    retranslateUi();
}

// ===== Password view =====

void PasswordFlow::initPassword() {
    m_password_text = new catalog::Label(catalog::LabelRole::Body);
    m_password_text->setTone(catalog::LabelTone::Secondary);
    m_password_text->setWordWrap(true);
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_confirm_input = new catalog::Input;
    m_confirm_input->setEchoMode(QLineEdit::Password);
    m_confirm_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_short =
        new catalog::FieldNote(TR("keys-password-short"), catalog::FieldNote::Tone::Error);
    m_password_mismatch =
        new catalog::FieldNote(TR("keys-password-no-match"), catalog::FieldNote::Tone::Error);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_password_actions = new catalog::ModalActions;
    m_password_actions->setBack(catalog::ModalActions::Back::Back);
    m_create = catalog::btnCreate();
    m_password_actions->addAction(m_create);
    m_password_fields = new catalog::FieldGroup;
}

void PasswordFlow::connectPassword() {
    for (auto *input : {m_password_input, m_confirm_input}) {
        connect(input, &QLineEdit::textEdited, this, &PasswordFlow::onPasswordEdited,
                qontrol::UNIQUE);
    }
    connect(m_confirm_input, &QLineEdit::returnPressed, this, &PasswordFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_create, &QPushButton::clicked, this, &PasswordFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_password_actions, &catalog::ModalActions::backClicked, this,
            &PasswordFlow::onBackClicked, qontrol::UNIQUE);
}

auto PasswordFlow::viewPassword() -> QWidget * {
    m_password_fields->addField(TR("keys-password-field"), m_password_input)
        ->addNote(m_password_short)
        ->addField(TR("keys-password-confirm-field"), m_confirm_input)
        ->addNote(m_password_mismatch);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_password_text)
        ->push(m_password_fields)
        ->push(m_password_error);
}

void PasswordFlow::showPassword() {
    m_view = View::Password;
    onPasswordEdited();
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_password);
    viewSlots()->setFooter(m_password_actions);
    emit viewChanged(TR("state-password-title"), ModalWidth::M);
    m_password_input->setFocus();
}

void PasswordFlow::hidePassword() {
    viewSlots()->keep(m_password);
    viewSlots()->keep(m_password_actions);
}

void PasswordFlow::createPassword() {
    if (!m_create->isEnabled()) {
        return;
    }
    auto password = m_password_input->text().toStdString();
    clearPassword();
    m_password_error->setVisible(false);
    showBusy();
    m_result = std::make_shared<QString>();
    // Only owned values cross the thread boundary; Qt drops delivery to a deleted flow.
    auto *worker = QThread::create([result = m_result, password = std::move(password)]() mutable {
        *result = QString::fromStdString(std::string(::state_create_with_password(password)));
        std::ranges::fill(password, '\0');
    });
    connect(worker, &QThread::finished, this, &PasswordFlow::onPasswordCreated,
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(worker, &QThread::finished, worker, &QObject::deleteLater, qontrol::UNIQUE);
    worker->start();
}

void PasswordFlow::clearPassword() {
    m_password_input->clear();
    m_confirm_input->clear();
}

// ===== Busy view =====

void PasswordFlow::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

auto PasswordFlow::viewBusy() -> catalog::ModalStatus * {
    return m_busy;
}

void PasswordFlow::showBusy() {
    m_view = View::Busy;
    emit busyChanged(true);
    m_busy->setTitle(TR("state-password-sealing"));
    m_busy->setSubtitle(TR("state-password-sealing-sub"));
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_busy);
    viewSlots()->setFooter(nullptr);
    emit viewChanged(TR("state-password-title"), ModalWidth::M);
}

void PasswordFlow::hideBusy() {
    viewSlots()->keep(m_busy);
}

// ===== Shared =====

void PasswordFlow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void PasswordFlow::retranslateUi() {
    m_password_text->setText(TR("state-password-text"));
    m_password_fields->setFieldLabel(m_password_input, TR("keys-password-field"));
    m_password_fields->setFieldLabel(m_confirm_input, TR("keys-password-confirm-field"));
    m_password_short->setText(TR("keys-password-short"));
    m_password_mismatch->setText(TR("keys-password-no-match"));
    m_create->setText(TR("btn-create"));
}

// ===== Slots =====

void PasswordFlow::onBackClicked() {
    switch (m_view) {
    case View::Password:
        clearPassword();
        emit back();
        break;
    case View::Busy:
        break;
    }
}

void PasswordFlow::onNextClicked() {
    switch (m_view) {
    case View::Password:
        createPassword();
        break;
    case View::Busy:
        break;
    }
}

void PasswordFlow::onPasswordEdited() {
    QString password = m_password_input->text();
    bool tooShort = !password.isEmpty() && password.size() < MIN_PASSWORD_LENGTH;
    bool mismatch = !m_confirm_input->text().isEmpty() && m_confirm_input->text() != password;
    m_password_short->setVisible(tooShort);
    m_password_mismatch->setVisible(mismatch);
    QString reason;
    if (password.isEmpty()) {
        reason = TR("keys-password-choose");
    } else if (password.size() < MIN_PASSWORD_LENGTH) {
        reason = TR("keys-password-short");
    } else if (m_confirm_input->text() != password) {
        reason = TR("keys-password-must-match");
    }
    m_create->setEnabled(reason.isEmpty());
    m_password_actions->setReason(reason);
}

void PasswordFlow::onPasswordCreated() {
    QString error = *m_result;
    m_result.reset();
    emit busyChanged(false);
    if (error.isEmpty()) {
        emit done();
        return;
    }
    m_password_error->setText(error);
    m_password_error->setVisible(true);
    showPassword();
}

} // namespace state
