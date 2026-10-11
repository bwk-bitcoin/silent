#pragma once

#include "theme/Palette.h"
#include <QScrollArea>

namespace catalog {

// A scrolling region, as the design's ScrollArea: the content sits inset by a
// scrollbar-wide gutter on both sides plus a small padding, and thin lines mark the
// top and bottom edges only while the content overflows. The gutters are kept
// whether the scrollbar shows or not, so nothing shifts when it comes or goes.
class ScrollArea : public QScrollArea {
    Q_OBJECT

public:
    explicit ScrollArea(QWidget *parent = nullptr);
    // Fitting, the area asks for its whole content height, wrapped text included, so
    // a list takes its height and scrolls only once its parent runs out of room.
    void setFitContent(bool fit);
    [[nodiscard]] auto sizeHint() const -> QSize override;
    [[nodiscard]] auto minimumSizeHint() const -> QSize override;
    [[nodiscard]] auto hasHeightForWidth() const -> bool override;
    [[nodiscard]] auto heightForWidth(int width) const -> int override;
    static auto qss(const Palette &p) -> QString;

    static constexpr int EDGE_LINE = 1;

    // Width of the scrollbar, the design's 10px: the width of each gutter.
    static constexpr auto scrollBarWidth() -> int {
        return resolve(Spacing::S);
    }

    // Space between the area's edge and its content. Across the scroll axis, a
    // scrollbar-wide gutter and a padding on each side (the right gutter is the bar
    // itself); along it, only the room of the edge line, so the content runs to the
    // edges. Content laid out next to an area aligns with its content by these.
    static constexpr auto insetH() -> int {
        return scrollBarWidth() + resolve(Spacing::XS);
    }

    static constexpr auto insetV() -> int {
        return EDGE_LINE;
    }

public slots:
    // Edge lines show only while the content overflows.
    void onRangeChanged(int minimum, int maximum);

protected:
    auto event(QEvent *event) -> bool override;
    auto viewportEvent(QEvent *event) -> bool override;
    auto eventFilter(QObject *watched, QEvent *event) -> bool override;

private:
    // The right gutter is the scrollbar's, or a margin when the bar is turned off.
    void applyMargins();
    [[nodiscard]] auto barShown() const -> bool;
    // The content's size, held to the width set on it.
    [[nodiscard]] auto contentSize() const -> QSize;

    bool m_fit = false;
    bool m_overflow = false;
};

} // namespace catalog
