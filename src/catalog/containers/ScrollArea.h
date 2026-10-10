#pragma once

#include "theme/Palette.h"
#include <QScrollArea>

namespace catalog {

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

    // Visible width of the scrollbar track.
    static constexpr auto scrollBarWidth() -> int {
        return resolve(Spacing::S);
    }

    // Gap kept between the scrolled content and the scrollbar so rows do not sit
    // flush against it.
    static constexpr auto scrollBarGap() -> int {
        return resolve(Spacing::XS);
    }

    // Total space the scrollbar reserves (bar + gap). Useful for laying out
    // content next to a scroll area so it aligns with the inset viewport.
    static constexpr auto scrollBarThickness() -> int {
        return scrollBarWidth() + scrollBarGap();
    }

public slots:
    // Edge lines show only while the content overflows.
    void onRangeChanged(int minimum, int maximum);

private:
    bool m_fit = false;
};

} // namespace catalog
