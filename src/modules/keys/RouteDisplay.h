#pragma once

#include <QIcon>
#include <QString>

#include <silent.h>

namespace keys {

// Shared between KeysTab (route strip icon + tooltip) and KeyModal (route list icon +
// label), both of which need the same KeyRouteKind -> icon/text mapping.
auto routeIcon(KeyRouteKind kind) -> QIcon;
auto routeLabel(KeyRouteKind kind) -> QString;

} // namespace keys
