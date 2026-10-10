#include "catalog/display/RouteIcons.h"

#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QLabel>
#include <Qontrol>

namespace catalog {

auto routeIcon(KeyRouteKind kind) -> IconFn {
    switch (kind) {
    case KeyRouteKind::Hot:
        return icon::key;
    case KeyRouteKind::Usb:
        return icon::usb;
    case KeyRouteKind::Qr:
        return icon::qr;
    case KeyRouteKind::SdCard:
        return icon::sdCard;
    default:
        return icon::key;
    }
}

auto routeLabel(KeyRouteKind kind) -> QString {
    switch (kind) {
    case KeyRouteKind::Hot:
        return TR("keys-route-hot");
    case KeyRouteKind::Usb:
        return TR("keys-route-usb");
    case KeyRouteKind::Qr:
        return TR("keys-route-qr");
    case KeyRouteKind::SdCard:
        return TR("keys-route-sd");
    default:
        return QString();
    }
}

RouteIcons::RouteIcons(QWidget *parent) : QWidget(parent) {
    m_row = new qontrol::Row;
    m_row->into(this);
}

void RouteIcons::setRoutes(const QList<KeyRouteKind> &routes) {
    // clear() also resets the layout spacing
    m_row->clear();
    m_row->spacing(resolve(Spacing::XS));
    for (int index = 0; index < routes.size(); ++index) {
        KeyRouteKind kind = routes.at(index);
        auto *glyph = new QLabel;
        glyph->setPixmap(routeIcon(kind)(IconColor::Secondary)
                             .pixmap(metric::ROUTE_ICON_SIZE, metric::ROUTE_ICON_SIZE));
        glyph->setToolTip(index == 0 ? TR("route-primary-tooltip").arg(routeLabel(kind))
                                     : routeLabel(kind));
        m_row->push(glyph);
        // Added to a shown row, it would only show on the next event loop pass.
        glyph->show();
    }
}

} // namespace catalog
