#include "views/DashboardLayout.h"

#include "catalog/display/Label.h"
#include "theme/Palette.h"
#include <QResizeEvent>
#include <Qontrol>

namespace {
// silent-design DashboardLayout constants.
const int DASH_MIN_W = DashboardLayout::MIN_CONTENT_WIDTH; // min single-column / block width
const int DASH_COL_MAX = 1140;                             // 1.5x min: single-column cap
const int DASH_MAX_W = 1600;                               // two-column total cap
const int DASH_GUTTER = 49;                                // 1px rule + 2x space-l
const int DASH_SPLIT_AT = 2 * DASH_MIN_W + DASH_GUTTER;    // 1569: columns fit side by side
const int DASH_WIDE_AT = DASH_SPLIT_AT;                    // 1569: top gap appears
} // namespace

DashboardLayout::DashboardLayout(const QString &title, QWidget *content, bool two_column,
                                 QWidget *parent, QWidget *title_right)
    : catalog::ScrollArea(parent),
      m_two_column(two_column) {
    // Page scroll like the design's overflow-y:auto; overflow-x is hidden (the
    // block is clamped to a min width and clips rather than scrolling sideways).
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *titleRow = (new qontrol::Row)
                         ->spacing(resolve(Spacing::M))
                         ->push(new catalog::Label(title, catalog::LabelRole::Title))
                         ->pushSpacer();
    if (title_right != nullptr) {
        titleRow->push(title_right);
    }
    m_block = (new qontrol::Column)->push(titleRow)->pushSpacer(resolve(Spacing::M))->push(content);

    // Centre horizontally (margin:0 auto) and pin to the top (top-aligned block).
    int pad = resolve(Spacing::S);
    m_centered_row = (new qontrol::Row)
                         ->margins(pad, pad, pad, pad)
                         ->pushSpacer()
                         ->push(m_block, 1000, Qt::AlignTop)
                         ->pushSpacer();

    setWidget(m_centered_row);
    applyMetrics();
}

void DashboardLayout::resizeEvent(QResizeEvent *event) {
    catalog::ScrollArea::resizeEvent(event);
    applyMetrics();
}

void DashboardLayout::applyMetrics() {
    int w = width();
    // sideBySide only when this is a two-column screen and there is room; the wide
    // top gap appears at the same threshold (design: DASH_SPLIT_AT == DASH_WIDE_AT).
    bool sideBySide = m_two_column && w >= DASH_SPLIT_AT;
    bool wide = w >= DASH_WIDE_AT;

    m_block->setMinimumWidth(sideBySide ? DASH_SPLIT_AT : DASH_MIN_W);
    m_block->setMaximumWidth(sideBySide ? DASH_MAX_W : DASH_COL_MAX);

    int pad = resolve(Spacing::S);
    int top = pad + (wide ? resolve(Spacing::XXL) : 0);
    m_centered_row->margins(pad, top, pad, pad);
}
