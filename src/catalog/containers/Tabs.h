#pragma once

#include <QTabWidget>

namespace catalog {

class Tabs : public QTabWidget {
    Q_OBJECT

public:
    explicit Tabs(QWidget *parent = nullptr);
    // The tabs' width around a page `page_width` wide: the pane's frame on both sides, as
    // the style lays a page out.
    [[nodiscard]] auto widthAround(int page_width) const -> int;
};

} // namespace catalog
