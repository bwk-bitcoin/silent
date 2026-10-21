#include "catalog/panels/home/HistoryTable.h"

#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/form/LabelCell.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QLabel>

namespace catalog {

namespace {

const int DIRECTION_W = 16;
const int DOTS_W = 13;
const int TXID_W = 175;
const int AMOUNT_W = 130;

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

auto confirmationIndicator(const RustTx &tx, uint32_t tip) -> QWidget * {
    auto status = toQString(tx.status);
    bool failed = status == QStringLiteral("verify-failed");
    if (failed || status == QStringLiteral("confirmed-unverified")) {
        auto *warning = new QLabel;
        warning->setPixmap(
            icon::alert(failed ? IconColor::Error : IconColor::Warning).pixmap(DOTS_W, DOTS_W));
        warning->setToolTip((failed ? TR("tx-status-verify-failed") : TR("tx-status-unverified"))
                                .arg(TR("tx-status-block").arg(tx.height)));
        return warning;
    }
    return new ConfirmDots(txConfirmations(tx.height, tip));
}

} // namespace

HistoryTable::HistoryTable(bool compact, QWidget *parent)
    : RowClickTable(parent),
      m_compact(compact) {
    addColumn({.width = DIRECTION_W, .icon = true});
    addColumn({.width = DOTS_W, .icon = true});
    addColumn({.title = TR("history-date"), .width = dateColumnWidth(compact)});
    addColumn({.title = TR("history-txid"), .width = TXID_W});
    addColumn({.title = TR("history-label"), .grow = true});
    addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    addColumn({.title = TR("history-amount"),
               .width = AMOUNT_W,
               .alignment = Qt::AlignRight,
               .headerAlignment = Qt::AlignRight});
}

void HistoryTable::addEntry(const RustTx &tx, uint32_t tip) {
    QString txid = toQString(tx.txid);
    bool unconfirmed = tx.height == 0;

    auto *txidLabel = new Label(shortenHash(txid), LabelRole::Mono);
    txidLabel->setProperty("tblmono", "secondary");
    txidLabel->setToolTip(txid);

    auto *label = labelCell(txid, toQString(tx.label));
    addClickableRow(txid, {new DirectionIcon(txDirection(toQString(tx.direction))),
                           confirmationIndicator(tx, tip),
                           dateStamp(tx.timestamp, unconfirmed, m_compact), txidLabel, label,
                           label->takeEditButton(), amountLabel(tx.amount, tx.amount_known)});
}

} // namespace catalog
