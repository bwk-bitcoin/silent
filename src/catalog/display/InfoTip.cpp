#include "catalog/display/InfoTip.h"

#include "theme/Icon.h"

namespace catalog {

namespace {

const int INFO_SIZE = 14;

} // namespace

InfoTip::InfoTip(const QString &text, QWidget *parent) : QLabel(parent) {
    setPixmap(icon::info(IconColor::Muted).pixmap(INFO_SIZE, INFO_SIZE));
    setToolTip(text);
    setCursor(Qt::WhatsThisCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

} // namespace catalog
