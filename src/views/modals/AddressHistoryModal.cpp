#include "AddressHistoryModal.h"

#include "../utils.h"
#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "catalog/panels/receive/IndexPill.h"
#include "catalog/panels/receive/LabelRow.h"
#include "catalog/panels/receive/QrCode.h"
#include "i18n/Tr.h"

#include <QGuiApplication>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSizePolicy>
#include <common.h>

namespace modal {

using catalog::Label;
using catalog::LabelRole;

namespace {
constexpr int QR_SIZE = 256;
constexpr double MAX_HEIGHT_FRAC = 0.7; // grow up to 70% of the screen, then scroll
} // namespace

AddressHistoryModal::AddressHistoryModal(const QString &title,
                                         const QList<QPair<QString, QString>> &entries,
                                         QWidget *parent) {
    Q_UNUSED(parent);
    setWindowTitle(title);
    build(title, entries);
}

void AddressHistoryModal::build(const QString &title,
                                const QList<QPair<QString, QString>> &entries) {
    // Header: just the title; the window frame provides the close button.
    auto *header = (new qontrol::Row)->push(new Label(title, LabelRole::Heading))->pushSpacer();

    // "Index" eyebrow column header.
    auto *eyebrow =
        (new qontrol::Row)->push(new Label(TR("receive-index"), LabelRole::Eyebrow))->pushSpacer();

    auto *list = new qontrol::Column;
    if (entries.isEmpty()) {
        list->push(new Label(TR("receive-no-previous-addresses"), LabelRole::Caption));
    }

    for (int i = 0; i < entries.size(); ++i) {
        const QString &addr = entries.at(i).first;
        const QString &label = entries.at(i).second;
        const int derivation = entries.size() - 1 - i; // newest highest (BIP-style)

        auto *pill = new catalog::IndexPill;
        pill->setIndex(derivation);
        pill->setProperty("rowIndex", i);
        connect(pill, &catalog::IndexPill::clicked, this, &AddressHistoryModal::onPillClicked,
                qontrol::UNIQUE);
        m_pills.append(pill);

        auto *verifyBtn = catalog::btnVerifyOnDeviceSm();
        verifyBtn->setToolTip(TR("receive-verify-tooltip"));
        verifyBtn->setProperty("address", addr);
        connect(verifyBtn, &QPushButton::clicked, this, &AddressHistoryModal::onVerifyClicked,
                qontrol::UNIQUE);

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
                               ->push(pill)
                               ->pushSpacer(resolve(Spacing::M))
                               ->push(verifyBtn)
                               ->pushSpacer(resolve(Spacing::M))
                               ->push(addressStack)
                               ->pushSpacer(resolve(Spacing::M))
                               ->push(copyBtn);

        // Hidden QR + label-editor expansion, revealed by the index pill.
        auto *qr = new catalog::QrCode;
        qr->setData(addr);
        qr->setFixedSize(QR_SIZE, QR_SIZE);
        auto *qrRow = (new qontrol::Row)->pushSpacer()->push(qr)->pushSpacer();

        auto *labelRow = new catalog::LabelRow;
        labelRow->setLabel(label);
        labelRow->setProperty("address", addr);
        connect(labelRow, &catalog::LabelRow::labelChanged, this,
                &AddressHistoryModal::onLabelChanged, qontrol::UNIQUE);
        auto *labelRowWrap = (new qontrol::Row)->pushSpacer()->push(labelRow)->pushSpacer();

        auto *expansion = new qontrol::Column;
        expansion->layout()->setContentsMargins(0, resolve(Spacing::M), 0, 0);
        expansion->push(qrRow)->pushSpacer(resolve(Spacing::M))->push(labelRowWrap);
        expansion->setVisible(false);
        m_expansions.append(expansion);

        auto *rowBlock = new qontrol::Column;
        rowBlock->push(new catalog::Separator)
            ->pushSpacer(resolve(Spacing::XS))
            ->push(rowContent)
            ->push(expansion)
            ->pushSpacer(resolve(Spacing::XS));
        list->push(rowBlock);
        m_rows.append(rowBlock);
    }

    m_scroll = new catalog::ScrollArea;
    m_scroll->setWidget(list);
    // Report the content height so the dialog grows with it; only scroll once it
    // would exceed most of the screen.
    m_scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    int screenHeight = QGuiApplication::primaryScreen()->availableGeometry().height();
    m_scroll->setMaximumHeight(static_cast<int>(screenHeight * MAX_HEIGHT_FRAC));

    auto *content = new qontrol::Column;
    content->push(header)
        ->pushSpacer(resolve(Spacing::M))
        ->push(eyebrow)
        ->pushSpacer(resolve(Spacing::XS))
        ->push(m_scroll);
    content->setMinimumWidth(resolve(Size::XXL));

    // Design modal padding is space-m (20px), matching Padding::L.
    setMainWidget(margin(content, resolve(Padding::L)));
}

void AddressHistoryModal::onPillClicked() {
    auto *pill = qobject_cast<catalog::IndexPill *>(sender());
    if (pill == nullptr) {
        return;
    }
    const int idx = pill->property("rowIndex").toInt();
    if (idx < 0 || idx >= m_expansions.size()) {
        return;
    }
    if (m_open == idx) {
        m_expansions[idx]->setVisible(false);
        m_pills[idx]->setActive(false);
        m_open = -1;
    } else {
        if (m_open >= 0 && m_open < m_expansions.size()) {
            m_expansions[m_open]->setVisible(false);
            m_pills[m_open]->setActive(false);
        }
        m_expansions[idx]->setVisible(true);
        m_pills[idx]->setActive(true);
        m_open = idx;
    }
    // Regrow the dialog and bring the expanded row to the top of the viewport so
    // its QR is visible; queued so the layout settles first.
    QMetaObject::invokeMethod(this, "onScrollToOpen", Qt::QueuedConnection);
}

void AddressHistoryModal::onScrollToOpen() {
    // Grow (or shrink) the dialog to fit the now-(collapsed/expanded) content,
    // capped by the scroll area's max height.
    adjustSize();
    if (m_open >= 0) {
        // Align after the resize has propagated (scrollbar range updated).
        QMetaObject::invokeMethod(this, "onScrollAlign", Qt::QueuedConnection);
    }
}

void AddressHistoryModal::onScrollAlign() {
    if (m_open < 0 || m_open >= m_rows.size() || m_scroll == nullptr) {
        return;
    }
    m_scroll->verticalScrollBar()->setValue(m_rows[m_open]->y());
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
