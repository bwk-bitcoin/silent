#pragma once

#include <QList>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QWidget>

namespace catalog {

class TxFlowCard;

class TxFlow : public QWidget {
    Q_OBJECT

public:
    enum class OutputKind { Recipient, Change, Fee };

    struct InputItem {
        double weight = 0.0;
        QString title;
        QString fiat;
        QString addr;
    };

    struct OutputItem {
        OutputKind kind = OutputKind::Recipient;
        double weight = 0.0;
        QString title;
        QString label;
        QString fiat;
        QString addr;
    };

    explicit TxFlow(QWidget *parent = nullptr);
    ~TxFlow() override;
    void setInputs(const QList<InputItem> &inputs);
    void setOutputs(const QList<OutputItem> &outputs);

protected:
    void paintEvent(QPaintEvent *event) override;
    [[nodiscard]] auto sizeHint() const -> QSize override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

    // Result of hit-testing the cursor against ribbons and caps. index is -1 when
    // nothing is hovered; is_input selects the input vs output collections.
    struct HoverHit {
        int index = -1;
        bool is_input = false;
    };

    // Map the cursor to virtual canvas space and find the band under it.
    [[nodiscard]] auto hitTest(const QPointF &pos) const -> HoverHit;
    // Float the hover card over the top-level window near the cursor, clamped to
    // the window edges.
    void positionCard(QMouseEvent *event);

private:
    // Recompute bands, ribbon paths and cap rects from the current items; reused
    // by paintEvent and by hit-testing so mouseMove never needs a paint.
    void recomputeGeometry();

    QList<InputItem> m_inputs;
    QList<OutputItem> m_outputs;

    QList<QPainterPath> m_input_ribbons;
    QList<QPainterPath> m_output_ribbons;
    QList<QRectF> m_input_caps;
    QList<QRectF> m_output_caps;

    int m_hover_index = -1;
    bool m_hover_is_input = false;

    TxFlowCard *m_card = nullptr;
};

} // namespace catalog
