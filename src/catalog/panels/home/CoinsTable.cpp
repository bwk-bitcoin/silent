#include "catalog/panels/home/CoinsTable.h"

#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/form/LabelCell.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

namespace catalog {

namespace {

const int TYPE_W = 58; // the design's 4.4em at the body font
const int OUTPOINT_W = 175;
const int VALUE_W = 130;

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

auto shortenOutpoint(const QString &outpoint) -> QString {
    int colon = static_cast<int>(outpoint.lastIndexOf(':'));
    if (colon < 0) {
        return shortenHash(outpoint);
    }
    return shortenHash(outpoint.left(colon)) + outpoint.mid(colon);
}

} // namespace

CoinsTable::CoinsTable(bool compact, QWidget *parent) : RowClickTable(parent), m_compact(compact) {
    addColumn({.title = TR("coins-type"), .width = TYPE_W});
    addColumn({.title = TR("coins-date"), .width = dateColumnWidth(compact)});
    addColumn({.title = TR("coins-outpoint"), .width = OUTPOINT_W});
    addColumn({.title = TR("coins-label"), .grow = true});
    addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    addColumn({.title = TR("coins-value"),
               .width = VALUE_W,
               .alignment = Qt::AlignRight,
               .headerAlignment = Qt::AlignRight});
}

void CoinsTable::addCoin(const RustCoin &coin) {
    QString outpoint = toQString(coin.outpoint);

    auto *outpointLabel = new Label(shortenOutpoint(outpoint), LabelRole::Mono);
    outpointLabel->setProperty("tblmono", "secondary");
    outpointLabel->setToolTip(outpoint);

    auto *label = labelCell(outpoint, toQString(coin.label));
    addClickableRow(outpoint,
                    {new Label(toQString(coin.account_type), LabelRole::Body),
                     dateStamp(coin.timestamp, coin.height == 0, m_compact), outpointLabel, label,
                     label->takeEditButton(), amountLabel(coin.value)});
}

} // namespace catalog
