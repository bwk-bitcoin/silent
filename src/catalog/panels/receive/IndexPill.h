#pragma once

#include "catalog/Button.h"

namespace catalog {

// The QR glyph + derivation index handle of an address row: an Index role button.
// setActive() keeps it pressed while its QR is expanded.
class IndexPill : public Button {
    Q_OBJECT

public:
    explicit IndexPill(QWidget *parent = nullptr);
    void setIndex(int index);
};

} // namespace catalog
