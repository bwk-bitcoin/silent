#include "AddressRow.h"

#include "QrCode.h"
#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "catalog/panels/receive/LabelRow.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QLabel>
#include <Qontrol>
#include <common.h>

namespace catalog {

AddressRow::AddressRow(QWidget *parent) : QWidget(parent) {
    setProperty("class", "address-row");

    m_address = new ChunkedAddress(this);
    m_qr_code = new QrCode(this);
    m_label = new LabelRow(this);
    m_placeholder = new Label(LabelRole::Body, this);
    m_placeholder->setTone(LabelTone::Muted);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_copy = btnCopyAddress();
    m_generate = btnGenerate();
    m_generate_new = btnGenerateNew();
    m_history = btnAwaitingPayment(0);
    m_verify = btnVerifyOnDevice();
    m_verify->setToolTip(TR("receive-verify-tooltip"));

    // Empty state: the centered placeholder over Generate.
    m_empty_state = (new qontrol::Column)
                        ->spacing(resolve(Spacing::M))
                        ->pushAligned(m_placeholder, Qt::AlignHCenter)
                        ->pushAligned(m_generate, Qt::AlignHCenter);

    // Address row: <device/verify button> <chunked address> <copy button>,
    // centered (design Receive AddressRow). Hidden as a whole when there is no
    // address yet.
    m_address_row = (new qontrol::Row)
                        ->spacing(resolve(Spacing::M))
                        ->pushSpacer()
                        ->push(m_verify)
                        ->push(m_address)
                        ->push(m_copy)
                        ->pushSpacer();

    // Secondary actions below: generate-new and awaiting-payment history.
    m_extra_actions = (new qontrol::Row)
                          ->spacing(resolve(Spacing::L))
                          ->pushSpacer()
                          ->push(m_generate_new)
                          ->push(m_history)
                          ->pushSpacer();

    auto *noteIcon = new QLabel(this);
    noteIcon->setPixmap(icon::info(IconColor::Muted)
                            .pixmap(metric::FIELD_NOTE_ICON_SIZE, metric::FIELD_NOTE_ICON_SIZE));
    m_verify_note_text = new Label(LabelRole::Caption, this);
    m_verify_note_text->setAlignment(Qt::AlignCenter);
    m_verify_note = (new qontrol::Row)
                        ->spacing(resolve(Spacing::XS))
                        ->pushSpacer()
                        ->push(noteIcon)
                        ->push(m_verify_note_text)
                        ->pushSpacer();
    m_verify_note->setVisible(false);

    // Outer column. Layout spacing (not fixed spacers) so the gap collapses when
    // a section is hidden by applyVisibility().
    (new qontrol::Column)
        ->spacing(resolve(Spacing::S))
        ->push(m_empty_state)
        ->push(m_qr_code, 0, Qt::AlignHCenter)
        ->push(m_label, 0, Qt::AlignHCenter)
        ->push(m_address_row)
        ->push(m_extra_actions)
        ->push(m_verify_note)
        ->into(this);

    m_verify->setVisible(false); // shown via setVerifyVisible()
    applyVisibility();           // default (no address): only placeholder shows

    connect(m_copy, &Button::clicked, this, &AddressRow::onCopyClicked, qontrol::UNIQUE);
    connect(m_generate, &Button::clicked, this, &AddressRow::onGenerateClicked, qontrol::UNIQUE);
    connect(m_generate_new, &Button::clicked, this, &AddressRow::onGenerateClicked,
            qontrol::UNIQUE);
    connect(m_history, &Button::clicked, this, &AddressRow::onHistoryClicked, qontrol::UNIQUE);
    connect(m_verify, &Button::clicked, this, &AddressRow::onVerifyClicked, qontrol::UNIQUE);
}

void AddressRow::setAddress(const QString &address) {
    m_address_text = address;
    m_address->setAddress(address);
    m_copy->setTextToCopy(address);
    m_qr_code->setData(address);
    applyVisibility();
}

void AddressRow::applyVisibility() {
    bool hasAddress = !m_address_text.isEmpty();
    // No address: show only the placeholder (and the Generate button below it);
    // the QR and the address row are hidden entirely (design).
    m_empty_state->setVisible(!hasAddress);
    m_qr_code->setVisible(hasAddress);
    m_address_row->setVisible(hasAddress);
    m_copy->setEnabled(hasAddress);
    // Label/history only show with an address; gated on their stored flags (not
    // isVisible(), which is unreliable during init). The device (verify) button
    // is owned by setVerifyVisible() and lives inside the (hidden-when-empty) row.
    m_label->setVisible(hasAddress && m_label_enabled);
    m_generate->setVisible(!hasAddress && m_generate_enabled);
    m_generate_new->setVisible(m_generate_enabled);
    m_history->setVisible(m_history_count > 0);
    m_extra_actions->setVisible(hasAddress && (m_generate_enabled || m_history_count > 0));
}

auto AddressRow::address() const -> QString {
    return m_address_text;
}

void AddressRow::setTwoLineAddress(bool two_lines) {
    m_address->setLineMode(two_lines ? ChunkedAddress::LineMode::Double
                                     : ChunkedAddress::LineMode::Single);
}

void AddressRow::setGenerateVisible(bool visible) {
    m_generate_enabled = visible;
    applyVisibility();
}

void AddressRow::setPlaceholderText(const QString &text) {
    m_placeholder->setText(text);
}

void AddressRow::setLabelText(const QString &text) {
    m_label->setLabel(text);
}

auto AddressRow::labelText() const -> QString {
    return m_label->label();
}

void AddressRow::setLabelVisible(bool visible) {
    m_label_enabled = visible;
    applyVisibility();
}

void AddressRow::setHistoryCount(int count) {
    m_history_count = count;
    m_history->setText(TR("btn-awaiting-payment").arg(count));
    applyVisibility();
}

void AddressRow::setVerifyVisible(bool visible) {
    m_verify->setVisible(visible);
}

void AddressRow::setVerifyNote(const QString &note) {
    m_verify_note_text->setTextWrapped(note, metric::ADDRESS_NOTE_MAX_WIDTH);
    m_verify_note->setVisible(!note.isEmpty());
}

void AddressRow::onCopyClicked() {
    // The clipboard write and check-mark flash are handled by the CopyButton.
    emit copyRequested(m_address_text);
}

void AddressRow::onGenerateClicked() {
    emit generateRequested();
}

void AddressRow::onHistoryClicked() {
    emit historyRequested();
}

void AddressRow::onVerifyClicked() {
    emit verifyRequested();
}

} // namespace catalog
