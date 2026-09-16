#include "RouteDisplay.h"

#include "i18n/Tr.h"
#include "theme/Icon.h"

namespace keys {

auto routeIcon(KeyRouteKind kind) -> QIcon {
    switch (kind) {
    case KeyRouteKind::Hot:
        return icon::key(IconColor::Accent);
    case KeyRouteKind::Usb:
        return icon::usb(IconColor::Accent);
    case KeyRouteKind::Qr:
        return icon::qr(IconColor::Accent);
    case KeyRouteKind::SdCard:
        return icon::sdCard(IconColor::Accent);
    default:
        return icon::key(IconColor::Muted);
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

} // namespace keys
