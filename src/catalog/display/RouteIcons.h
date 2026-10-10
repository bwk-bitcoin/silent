#pragma once

#include "theme/Icon.h"
#include <QList>
#include <QString>
#include <QWidget>

#include <silent.h>

namespace qontrol {
class Row;
}

namespace catalog {

// One icon and one label per route kind, everywhere a route appears.
auto routeIcon(KeyRouteKind kind) -> IconFn;
auto routeLabel(KeyRouteKind kind) -> QString;

// A key's route kinds as a row of icons, the first one being the primary; each
// icon's tooltip names its kind.
class RouteIcons : public QWidget {
    Q_OBJECT

public:
    explicit RouteIcons(QWidget *parent = nullptr);
    void setRoutes(const QList<KeyRouteKind> &routes);

private:
    qontrol::Row *m_row = nullptr;
};

} // namespace catalog
