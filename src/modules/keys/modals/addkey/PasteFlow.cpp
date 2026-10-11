#include "PasteFlow.h"

#include "KeyChecks.h"
#include "KeyFields.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QRegularExpression>

#include <Qontrol>
#include <common.h>

namespace keys::modal {

namespace {

// [73c5da0a/84'/1'/0']tpubD...: the origin, then the key.
const QRegularExpression PASTED_KEY_RE(QStringLiteral(R"(^(\[[^\]]*\])\s*(\S+)$)"));
const QRegularExpression ORIGIN_RE(QStringLiteral(R"(^\[([0-9a-f]{8})((?:/\d+['h]?)+)\]$)"));
const QRegularExpression XPUB_RE(QStringLiteral(R"(^[xt]pub[1-9A-HJ-NP-Za-km-z]{100,}$)"));

} // namespace

PasteFlow::PasteFlow(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

void PasteFlow::start() {
    onPasteChanged();
    showPaste();
}

void PasteFlow::init() {
    m_paste = new catalog::ValidatedInput(catalog::InputRole::Mono);
    m_paste->input()->setPlaceholderText(TR("keys-paste-placeholder")
                                             .arg(KeysModule::network() == Network::Bitcoin
                                                      ? QStringLiteral("xpub")
                                                      : QStringLiteral("tpub")));
    m_paste_note =
        new catalog::FieldNote(TR("keys-paste-invalid"), catalog::FieldNote::Tone::Error);
    m_paste_note->setVisible(false);
    m_paste_alias = aliasInput();
    m_paste_duplicate = duplicateNotice();
    m_paste_error = errorNotice();
    m_add_pasted = catalog::btnAddKey();
    m_paste_actions = new catalog::ModalActions;
    m_paste_actions->setBack(catalog::ModalActions::Back::Back);
    m_paste_actions->addAction(m_add_pasted);
}

void PasteFlow::doConnect() {
    connect(m_paste, &catalog::ValidatedInput::textChanged, this, &PasteFlow::onPasteChanged,
            qontrol::UNIQUE);
    connect(m_paste_alias, &QLineEdit::textEdited, this, &PasteFlow::onPasteChanged,
            qontrol::UNIQUE);
    connect(m_add_pasted, &QPushButton::clicked, this, &PasteFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_paste_actions, &catalog::ModalActions::backClicked, this, &PasteFlow::back,
            qontrol::UNIQUE);
}

void PasteFlow::view() {
    auto *fields = new catalog::FieldGroup;
    fields->addField(TR("keys-xpub"), m_paste)
        ->addNote(m_paste_note)
        ->addField(TR("keys-alias"), m_paste_alias);
    m_paste_body = (new qontrol::Column)
                       ->spacing(resolve(Spacing::S))
                       ->push(fields)
                       ->push(m_paste_duplicate)
                       ->push(m_paste_error);
}

void PasteFlow::showPaste() {
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_paste_body);
    viewSlots()->setFooter(m_paste_actions);
    emit viewChanged(TR("keys-add-paste-title"), ModalWidth::L);
}

auto PasteFlow::pastedKey() const -> std::optional<PastedKey> {
    auto match = PASTED_KEY_RE.match(m_paste->input()->text().trimmed());
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    auto origin = ORIGIN_RE.match(match.captured(1));
    QString xpub = match.captured(2);
    if (!origin.hasMatch() || !XPUB_RE.match(xpub).hasMatch()) {
        return std::nullopt;
    }
    return PastedKey{.fingerprint = origin.captured(1),
                     .xpub = {.path = QStringLiteral("m") + origin.captured(2), .xpub = xpub}};
}

// ===== Slots =====

void PasteFlow::onNextClicked() {
    auto key = pastedKey();
    if (!key.has_value()) {
        return;
    }
    // A pasted xpub comes from an airgapped device, which signs over its SD card.
    QString error = addKey(m_module, key->fingerprint, m_paste_alias->text().trimmed(),
                           {KeyRouteKind::SdCard}, {key->xpub});
    if (!error.isEmpty()) {
        m_paste_error->setText(error);
        m_paste_error->setVisible(true);
        window()->adjustSize();
        return;
    }
    emit added();
}

void PasteFlow::onPasteChanged() {
    QString text = m_paste->input()->text().trimmed();
    auto key = pastedKey();
    if (text.isEmpty()) {
        m_paste->setState(catalog::ValidationMark::State::None);
    } else {
        m_paste->setState(key.has_value() ? catalog::ValidationMark::State::Valid
                                          : catalog::ValidationMark::State::Invalid);
    }
    m_paste_note->setVisible(!text.isEmpty() && !key.has_value());
    QString duplicate = key.has_value() ? registeredAlias(m_module, key->fingerprint) : QString();
    m_paste_duplicate->setText(
        TR("keys-duplicate-body").arg(key.has_value() ? key->fingerprint : QString(), duplicate));
    m_paste_duplicate->setVisible(!duplicate.isEmpty());
    QString reason;
    if (key.has_value()) {
        reason = duplicate.isEmpty() ? aliasReason(m_module, m_paste_alias->text().trimmed())
                                     : TR("keys-duplicate-reason");
    }
    m_add_pasted->setEnabled(key.has_value() && reason.isEmpty());
    m_paste_actions->setReason(reason);
    window()->adjustSize();
}

} // namespace keys::modal
