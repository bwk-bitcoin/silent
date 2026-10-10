#include "AddressHistoryModal.h"

#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/form/FieldNote.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "catalog/panels/receive/IndexPill.h"
#include "catalog/panels/receive/LabelRow.h"
#include "catalog/panels/receive/QrCode.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"

#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <common.h>

namespace modal {

using catalog::Label;
using catalog::LabelRole;

namespace {
constexpr int QR_SIZE = 256;
} // namespace

AddressHistoryModal::AddressHistoryModal(const QString &title,
                                         const QList<QPair<QString, QString>> &entries,
                                         bool verifiable, const QString &verify_note)
    : m_title(title),
      m_entries(entries),
      m_verifiable(verifiable),
      m_verify_note(verify_note) {
    init();
    doConnect();
    view();
}

void AddressHistoryModal::init() {
    for (int i = 0; i < m_entries.size(); ++i) {
        const QString &addr = m_entries.at(i).first;
        const QString &label = m_entries.at(i).second;
        int derivation = static_cast<int>(m_entries.size()) - 1 - i; // newest highest (BIP-style)

        auto *pill = new catalog::IndexPill;
        pill->setIndex(derivation);
        pill->setProperty("rowIndex", i);
        m_pills.append(pill);

        auto *verifyBtn = catalog::btnVerifyOnDeviceSm();
        verifyBtn->setToolTip(TR("receive-verify-tooltip"));
        verifyBtn->setProperty("address", addr);
        verifyBtn->setVisible(m_verifiable);
        m_verify_buttons.append(verifyBtn);

        auto *copyBtn = catalog::btnCopySm();
        copyBtn->setTextToCopy(addr);

        auto *chunked = new catalog::ChunkedAddress;
        chunked->setAddress(addr);

        auto *addressStack = new qontrol::Column;
        addressStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        if (!label.isEmpty()) {
            auto *labelText = new Label(label, LabelRole::Section);
            labelText->setTone(catalog::LabelTone::Secondary);
            labelText->setSize(catalog::LabelSize::Sm);
            addressStack->push(labelText)->pushSpacer(resolve(Spacing::XXS));
        }
        addressStack->push(chunked);

        auto *rowContent = (new qontrol::Row)
                               ->spacing(resolve(Spacing::M))
                               ->push(pill)
                               ->push(verifyBtn)
                               ->push(addressStack)
                               ->push(copyBtn);

        // Hidden QR + label-editor expansion, revealed by the index pill.
        auto *qr = new catalog::QrCode;
        qr->setData(addr);
        qr->setFixedSize(QR_SIZE, QR_SIZE);

        auto *labelRow = new catalog::LabelRow;
        labelRow->setLabel(label);
        labelRow->setProperty("address", addr);
        m_label_rows.append(labelRow);

        auto *expansion = (new qontrol::Column)
                              ->spacing(resolve(Spacing::M))
                              ->push(qr, 0, Qt::AlignHCenter)
                              ->push(labelRow, 0, Qt::AlignHCenter);
        expansion->layout()->setContentsMargins(0, resolve(Spacing::M), 0, 0);
        expansion->setVisible(false);
        m_expansions.append(expansion);

        auto *rowBlock = (new qontrol::Column)
                             ->push(new catalog::Separator)
                             ->pushSpacer(resolve(Spacing::XS))
                             ->push(rowContent)
                             ->push(expansion)
                             ->pushSpacer(resolve(Spacing::XS));
        m_rows.append(rowBlock);
    }
}

void AddressHistoryModal::doConnect() {
    for (auto *pill : m_pills) {
        connect(pill, &catalog::IndexPill::clicked, this, &AddressHistoryModal::onPillClicked,
                qontrol::UNIQUE);
    }
    for (auto *verifyBtn : m_verify_buttons) {
        connect(verifyBtn, &QPushButton::clicked, this, &AddressHistoryModal::onVerifyClicked,
                qontrol::UNIQUE);
    }
    for (auto *labelRow : m_label_rows) {
        connect(labelRow, &catalog::LabelRow::labelChanged, this,
                &AddressHistoryModal::onLabelChanged, qontrol::UNIQUE);
    }
}

void AddressHistoryModal::view() {
    auto *list = (new qontrol::Column)
                     ->push(new Label(TR("receive-index"), LabelRole::Eyebrow))
                     ->pushSpacer(resolve(Spacing::XS));
    if (!m_verify_note.isEmpty()) {
        list->push(
                new catalog::FieldNote(m_verify_note, catalog::FieldNote::Tone::Muted, icon::info))
            ->pushSpacer(resolve(Spacing::XS));
    }
    if (m_entries.isEmpty()) {
        list->push(new Label(TR("receive-no-previous-addresses"), LabelRole::Caption));
    }
    for (auto *row : m_rows) {
        list->push(row);
    }

    m_scroll = new catalog::ScrollArea;
    m_scroll->setFitContent(true);
    m_scroll->setWidget(list);
    setWindowTitle(m_title);
    setBody(m_scroll);
    setWidth(ModalWidth::XL);
}

void AddressHistoryModal::onPillClicked() {
    auto *pill = qobject_cast<catalog::IndexPill *>(sender());
    if (pill == nullptr) {
        return;
    }
    int idx = pill->property("rowIndex").toInt();
    if (idx < 0 || idx >= m_expansions.size()) {
        return;
    }
    if (m_open >= 0) {
        m_expansions[m_open]->setVisible(false);
        m_pills[m_open]->setActive(false);
    }
    m_open = m_open == idx ? -1 : idx;
    if (m_open >= 0) {
        m_expansions[m_open]->setVisible(true);
        m_pills[m_open]->setActive(true);
    }
    // Fit the window to the list again, then bring the open row's QR into view.
    adjustSize();
    if (m_open >= 0) {
        int top = m_rows[m_open]->mapTo(m_scroll->widget(), QPoint(0, 0)).y();
        m_scroll->verticalScrollBar()->setValue(top);
    }
}

void AddressHistoryModal::onVerifyClicked() {
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (btn == nullptr) {
        return;
    }
    emit verifyRequested(btn->property("address").toString());
    accept();
}

void AddressHistoryModal::onLabelChanged(const QString &label) {
    auto *row = qobject_cast<catalog::LabelRow *>(sender());
    if (row == nullptr) {
        return;
    }
    emit labelEdited(row->property("address").toString(), label);
}

} // namespace modal
