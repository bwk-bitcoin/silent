#pragma once

#include "catalog/panels/home/RowClickTable.h"

#include <silent.h>

namespace catalog {

// The Coins panel's table: type, date, outpoint, editable label and value. A row
// opens the transaction that created the coin, keyed by outpoint.
class CoinsTable : public RowClickTable {
    Q_OBJECT

public:
    // `compact` dates drop the time.
    explicit CoinsTable(bool compact, QWidget *parent = nullptr);
    void addCoin(const RustCoin &coin);

private:
    bool m_compact = false;
};

} // namespace catalog
