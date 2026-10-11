#pragma once

#include <QScrollBar>

namespace catalog {

// The scrollbar of a ScrollArea. It paints its handle and nothing else: no track,
// no groove, whatever the style and Qt version would draw under it, so the bar shows
// what lies behind it. With nothing to scroll it paints nothing at all.
class ScrollBar : public QScrollBar {
    Q_OBJECT

public:
    explicit ScrollBar(Qt::Orientation orientation, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    [[nodiscard]] auto handleRect() const -> QRect;

    bool m_hovered = false;
};

} // namespace catalog
