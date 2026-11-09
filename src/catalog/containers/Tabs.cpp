#include "catalog/containers/Tabs.h"

#include <QRect>
#include <QStyle>
#include <QStyleOptionTabWidgetFrame>

namespace catalog {

Tabs::Tabs(QWidget *parent) : QTabWidget(parent) {
    setProperty("class", "tabs");
}

auto Tabs::widthAround(int page_width) const -> int {
    // The stylesheet's pane frame applies once the tabs are polished.
    ensurePolished();
    QStyleOptionTabWidgetFrame option;
    initStyleOption(&option);
    // Laid out at that width, the page takes the width the frame leaves.
    option.rect = QRect(0, 0, page_width, height());
    int frame = page_width -
                style()->subElementRect(QStyle::SE_TabWidgetTabContents, &option, this).width();
    return page_width + frame;
}

} // namespace catalog
