#include "RevealMnemonic.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/WordGrid.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <Qontrol>

#include <common.h>

namespace keys::modal {

RevealMnemonic::RevealMnemonic(const QStringList &words) {
    setWindowTitle(TR("keys-reveal-title"));
    init(words);
    doConnect();
    view();
}

RevealMnemonic::~RevealMnemonic() {
    m_words->setWords({});
}

void RevealMnemonic::init(const QStringList &words) {
    m_words = new catalog::WordGrid;
    m_words->setWords(words);
    m_words->setMasked(false);
    m_hide_words = catalog::btnHideWords();
    m_show_words = catalog::btnShowWords();
    m_show_words->setVisible(false);
    m_done = catalog::btnDone();
}

void RevealMnemonic::doConnect() {
    connect(m_hide_words, &QPushButton::clicked, this, &RevealMnemonic::onToggleWordsClicked,
            qontrol::UNIQUE);
    connect(m_show_words, &QPushButton::clicked, this, &RevealMnemonic::onToggleWordsClicked,
            qontrol::UNIQUE);
    connect(m_done, &QPushButton::clicked, this, &QDialog::accept, qontrol::UNIQUE);
}

void RevealMnemonic::view() {
    auto *warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    warning->setTitle(TR("keys-reveal-warning-title"));
    warning->setText(TR("keys-reveal-warning"));
    auto *toggle = (new qontrol::Row)->pushSpacer()->push(m_hide_words)->push(m_show_words);
    auto *actions = new catalog::ModalActions;
    actions->addAction(m_done);
    setWideBody(
        (new qontrol::Column)
            ->spacing(resolve(Spacing::M))
            ->push(warning)
            ->push(
                (new qontrol::Column)->spacing(resolve(Spacing::S))->push(toggle)->push(m_words)));
    setFooter(actions);
    setWidth(ModalWidth::L);
}

void RevealMnemonic::onToggleWordsClicked() {
    bool masked = m_hide_words->isVisible();
    m_words->setMasked(masked);
    m_hide_words->setVisible(!masked);
    m_show_words->setVisible(masked);
}

} // namespace keys::modal
