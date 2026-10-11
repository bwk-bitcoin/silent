#include "RevealFlow.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/WordGrid.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "theme/Palette.h"

#include <QLineEdit>
#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::modal {

RevealFlow::RevealFlow(KeysModule *module, QString fingerprint)
    : KeyDetailFlow(module, std::move(fingerprint)) {
    init();
    doConnect();
    view();
}

RevealFlow::~RevealFlow() {
    m_password_input->clear();
    m_words->setWords({});
}

void RevealFlow::start() {
    showPassword();
}

void RevealFlow::init() {
    initPassword();
    initReveal();
}

void RevealFlow::doConnect() {
    connectPassword();
    connectReveal();
}

void RevealFlow::view() {
    m_password_body = viewPassword();
    m_reveal_body = viewReveal();
    hideReveal();
}

// ===== Password view =====

void RevealFlow::initPassword() {
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_password_proceed = catalog::btnShowMnemonic();
    m_password_unlocking = catalog::btnUnlocking();
    m_password_unlocking->setEnabled(false);
    m_password_unlocking->setVisible(false);
    m_password_actions = new catalog::ModalActions;
    m_password_actions->setBack(catalog::ModalActions::Back::Back);
    m_password_actions->addAction(m_password_unlocking);
    m_password_actions->addAction(m_password_proceed);
}

void RevealFlow::connectPassword() {
    connect(m_password_input, &QLineEdit::textEdited, this, &RevealFlow::onPasswordEdited,
            qontrol::UNIQUE);
    connect(m_password_input, &QLineEdit::returnPressed, this, &RevealFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_password_proceed, &QPushButton::clicked, this, &RevealFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_password_actions, &catalog::ModalActions::backClicked, this,
            &RevealFlow::onBackClicked, qontrol::UNIQUE);
    connect(module(), &KeysModule::mnemonicRevealed, this, &RevealFlow::onMnemonicRevealed,
            qontrol::UNIQUE);
}

auto RevealFlow::viewPassword() -> QWidget * {
    auto *body = new QWidget;
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(new catalog::Field(TR("flow-password"), m_password_input))
        ->push(m_password_error)
        ->into(body);
    return body;
}

// An empty field with the focus in it, and the error kept if one was just shown.
void RevealFlow::showPassword() {
    m_view = View::Password;
    m_password_input->clear();
    m_password_input->setEnabled(true);
    m_password_unlocking->setVisible(false);
    m_password_proceed->setVisible(true);
    onPasswordEdited();
    showHeader(TR("keys-reveal-title"));
    viewSlots()->setBody(m_password_body);
    viewSlots()->setFooter(m_password_actions);
    emit viewChanged(TR("keys-reveal-title"), ModalWidth::L);
    m_password_input->setFocus();
}

void RevealFlow::proceedPassword() {
    if (m_reveal_req != 0 || m_password_input->text().isEmpty()) {
        return;
    }
    QString password = m_password_input->text();
    m_password_input->clear();
    m_password_input->setEnabled(false);
    m_password_error->setVisible(false);
    m_password_proceed->setVisible(false);
    m_password_unlocking->setVisible(true);
    m_password_actions->setReason(QString());
    m_reveal_req = module()->nextReqId();
    module()->revealMnemonic(m_reveal_req, fingerprint(), password);
    password.fill(QChar());
    password.clear();
}

// ===== Reveal view =====

void RevealFlow::initReveal() {
    m_words = new catalog::WordGrid;
    m_hide_words = catalog::btnHideWords();
    m_show_words = catalog::btnShowWords();
    m_words_done = catalog::btnDone();
    m_reveal_actions = new catalog::ModalActions;
    m_reveal_actions->addAction(m_words_done);
}

void RevealFlow::connectReveal() {
    connect(m_hide_words, &QPushButton::clicked, this, &RevealFlow::onToggleWordsClicked,
            qontrol::UNIQUE);
    connect(m_show_words, &QPushButton::clicked, this, &RevealFlow::onToggleWordsClicked,
            qontrol::UNIQUE);
    connect(m_words_done, &QPushButton::clicked, this, &RevealFlow::onNextClicked, qontrol::UNIQUE);
}

auto RevealFlow::viewReveal() -> QWidget * {
    auto *warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    warning->setTitle(TR("keys-reveal-warning-title"));
    warning->setText(TR("keys-reveal-warning"));
    auto *toggle = (new qontrol::Row)->pushSpacer()->push(m_hide_words)->push(m_show_words);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(warning)
        ->push((new qontrol::Column)->spacing(resolve(Spacing::S))->push(toggle)->push(m_words));
}

void RevealFlow::showReveal() {
    m_view = View::Reveal;
    m_words->setMasked(false);
    m_hide_words->setVisible(true);
    m_show_words->setVisible(false);
    showHeader(TR("keys-reveal-title"));
    viewSlots()->setBody(m_reveal_body);
    viewSlots()->setFooter(m_reveal_actions);
    emit viewChanged(TR("keys-reveal-title"), ModalWidth::L);
}

void RevealFlow::hideReveal() {
    viewSlots()->keep(m_reveal_body);
    viewSlots()->keep(m_reveal_actions);
}

void RevealFlow::closeWords() {
    m_words->setWords({});
    emit back();
}

// ===== Slots =====

void RevealFlow::onBackClicked() {
    switch (m_view) {
    case View::Password:
        emit back();
        return;
    case View::Reveal:
        return;
    }
}

void RevealFlow::onNextClicked() {
    switch (m_view) {
    case View::Password:
        proceedPassword();
        return;
    case View::Reveal:
        closeWords();
        return;
    }
}

void RevealFlow::onPasswordEdited() {
    bool empty = m_password_input->text().isEmpty();
    if (!empty) {
        m_password_error->setVisible(false);
    }
    m_password_proceed->setEnabled(!empty);
    m_password_actions->setReason(empty ? TR("flow-password-reason") : QString());
}

void RevealFlow::onMnemonicRevealed(ReqId req_id, const MnemonicResult &result) {
    if (m_reveal_req == 0 || req_id != m_reveal_req) {
        return;
    }
    m_reveal_req = 0;
    if (!result.is_ok) {
        m_password_error->setText(
            result.wrong_password ? TR("flow-password-wrong")
                                  : TR("keys-reveal-failed").arg(KeysModule::toQt(result.error)));
        m_password_error->setVisible(true);
        showPassword();
        return;
    }
    // A QString cannot be zeroized: Rust owns the real wiping of the phrase.
    QString mnemonic = KeysModule::toQt(result.mnemonic);
    m_words->setWords(mnemonic.split(QLatin1Char(' '), Qt::SkipEmptyParts));
    mnemonic.fill(QChar());
    showReveal();
}

void RevealFlow::onToggleWordsClicked() {
    bool masked = m_hide_words->isVisible();
    m_words->setMasked(masked);
    m_hide_words->setVisible(!masked);
    m_show_words->setVisible(masked);
}

} // namespace keys::modal
