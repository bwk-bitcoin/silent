#pragma once

#include "catalog/containers/ScrollArea.h"

class QResizeEvent;

namespace qontrol {
class Row;
}

// Mirrors silent-design's DashboardLayout: a page-level vertical scroll holding a
// horizontally-centred, width-clamped, top-aligned content block. The block's
// min/max width and the wide-window top gap are recomputed from the live width
// (the Qt equivalent of the design's useMeasure-driven re-render).
class DashboardLayout : public catalog::ScrollArea {
    Q_OBJECT

public:
    // Minimum single-column content width (the block min).
    static constexpr int MIN_CONTENT_WIDTH = 760;

    // The narrowest the dashboard shows its block whole: the block at its minimum, its
    // padding on both sides, and the scroll area's gutters and bar around them. The main
    // window sizes itself to the sidebar plus this.
    static constexpr auto minimumWidth() -> int {
        return catalog::ScrollArea::widthAround(MIN_CONTENT_WIDTH + (2 * resolve(Spacing::S)));
    }

    DashboardLayout(const QString &title, QWidget *content, bool two_column = false,
                    QWidget *parent = nullptr, QWidget *title_right = nullptr);

protected:
    void resizeEvent(QResizeEvent *event) override;

    void applyMetrics();

private:
    bool m_two_column;
    QWidget *m_block = nullptr;
    qontrol::Row *m_centered_row = nullptr;
};
