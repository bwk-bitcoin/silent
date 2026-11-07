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

auto confirmationIndicator(const backend::Transaction &tx, uint32_t tip) -> QWidget * {
    auto height = static_cast<uint32_t>(tx.height.value_or(0));
    const QString &status = tx.status;
    bool failed = status == QStringLiteral("verify-failed");
    if (failed || status == QStringLiteral("confirmed-unverified")) {
        auto *warning = new QLabel;
        warning->setPixmap(
            icon::alert(failed ? IconColor::Error : IconColor::Warning).pixmap(DOTS_W, DOTS_W));
        warning->setToolTip((failed ? TR("tx-status-verify-failed") : TR("tx-status-unverified"))
                                .arg(TR("tx-status-block").arg(height)));
        return warning;
    }
    return new ConfirmDots(txConfirmations(height, tip));
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

void HistoryTable::addEntry(const backend::Transaction &tx, uint32_t tip) {
    const QString &txid = tx.txid;
    bool unconfirmed = !tx.height.has_value();

    auto *txidLabel = new Label(shortenHash(txid), LabelRole::Mono);
    txidLabel->setProperty("tblmono", "secondary");
    txidLabel->setToolTip(txid);

    auto *label = labelCell(txid, tx.label);
    addClickableRow(
        txid, {new DirectionIcon(txDirection(tx.direction)), confirmationIndicator(tx, tip),
               dateStamp(tx.timestamp.value_or(0), unconfirmed, m_compact), txidLabel, label,
               label->takeEditButton(), amountLabel(tx.amount.value_or(0), tx.amount.has_value())});
}

} // namespace catalog
