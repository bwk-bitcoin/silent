#include "catalog/panels/receive/IndexPill.h"

#include "i18n/Tr.h"
#include "theme/Icon.h"

namespace catalog {

namespace {
constexpr int QR_ICON_SIZE = 13;
} // namespace

IndexPill::IndexPill(QWidget *parent) : Button(ButtonRole::Index, parent) {
    setIcon(icon::qr());
    setIconSize(QSize(QR_ICON_SIZE, QR_ICON_SIZE));
    setToolTip(TR("receive-show-qr"));
}

void IndexPill::setIndex(int index) {
    setText(QString::number(index));
}

} // namespace catalog
