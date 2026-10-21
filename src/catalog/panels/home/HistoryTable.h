#pragma once

#include "catalog/panels/home/RowClickTable.h"

#include <cstdint>
#include <silent.h>

namespace catalog {

// The History panel's transaction table: direction, confirmations, date, txid,
// editable label and amount. A row opens its transaction, keyed by txid.
class HistoryTable : public RowClickTable {
    Q_OBJECT

public:
    // `compact` dates drop the time.
    explicit HistoryTable(bool compact, QWidget *parent = nullptr);
    // `tip` counts its confirmations, 0 when not known.
    void addEntry(const RustTx &tx, uint32_t tip);

private:
    bool m_compact = false;
};

} // namespace catalog
