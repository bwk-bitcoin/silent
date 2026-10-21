#pragma once

#include "catalog/containers/Card.h"
#include <QList>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QWidget>

class QHideEvent;

namespace catalog {

class Label;
class TxFlowHoverCard;

// The transaction shape: inputs flow into a trunk that splits into the outputs, each
// band as thick as its amount. Hovering a band shows its detail card. In detail mode
// (the transaction summary) each band carries its index and id instead of its kind.
class TxFlow : public QWidget {
    Q_OBJECT

public:
    enum class OutputKind { Recipient, Change, Fee };

    struct InputItem {
        double weight = 0.0;
        QString title;
        QString fiat;
        QString addr;
        // Detail mode: the index drawn in the band's cap and the id along the band.
        QString index;
        QString ref;
        // False when the amount is not known: the band is drawn at its minimum
        // thickness and the hover card says the value is unknown.
        bool value_known = true;
    };

    struct OutputItem {
        OutputKind kind = OutputKind::Recipient;
        double weight = 0.0;
        QString title;
        QString label;
        QString fiat;
        QString addr;
        // Detail mode: the index drawn in the band's cap and the id along the band.
        QString index;
        QString ref;
        // False when the amount is not known: the band is drawn at its minimum
        // thickness and the hover card says the value is unknown.
        bool value_known = true;
    };

    explicit TxFlow(QWidget *parent = nullptr);
    ~TxFlow() override;
    void setInputs(const QList<InputItem> &inputs);
    void setOutputs(const QList<OutputItem> &outputs);
    // Detail mode draws every item given, a zero or unknown amount at the minimum band
    // thickness; otherwise only the items with a positive weight are drawn.
    void setDetail(bool detail);
    // The drawn bands.
    [[nodiscard]] auto inputs() const -> const QList<InputItem> &;
    [[nodiscard]] auto outputs() const -> const QList<OutputItem> &;

public slots:
    // Hidden amounts hide the hover card's fiat in detail mode, at once.
    void onAmountModeChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    [[nodiscard]] auto sizeHint() const -> QSize override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    // Result of hit-testing the cursor against ribbons and caps. index is -1 when
    // nothing is hovered; is_input selects the input vs output collections.
    struct HoverHit {
        int index = -1;
        bool is_input = false;
    };

    // Maps widget pixels to the design canvas: the canvas is scaled down to fit the
    // widget, never up, and centered.
    struct CanvasMap {
        double width = 0.0;
        double scale = 1.0;
        double dx = 0.0;
        double dy = 0.0;
    };

    [[nodiscard]] auto canvasMap() const -> CanvasMap;
    [[nodiscard]] auto canvasHeight() const -> int;
    [[nodiscard]] auto hitTest(const QPointF &pos) const -> HoverHit;
    void paintEmpty(QPainter &painter);
    void paintLabels(QPainter &painter, double canvas_width);
    // Shows the hover card beside the cursor, kept within the diagram's width.
    void positionCard(const QPointF &pos);
    // Recompute bands, ribbon paths and cap rects from the current items; reused
    // by paintEvent and by hit-testing so mouseMove never needs a paint.
    void recomputeGeometry(double canvas_width);
    void clearHover();
    // Picks the drawn bands from the items given, for the current mode.
    void applyItems();
    void updateCard();
    // The hover card's value line: the fiat given, unknown, or nothing while hidden.
    [[nodiscard]] auto cardValue(bool value_known, const QString &fiat) const -> QString;

private:
    QList<InputItem> m_given_inputs;
    QList<OutputItem> m_given_outputs;
    QList<InputItem> m_inputs;
    QList<OutputItem> m_outputs;
    bool m_detail = false;

    QList<QPainterPath> m_input_ribbons;
    QList<QPainterPath> m_output_ribbons;
    QList<QRectF> m_input_caps;
    QList<QRectF> m_output_caps;
    double m_trunk_top = 0.0;
    double m_trunk_bot = 0.0;

    int m_hover_index = -1;
    bool m_hover_is_input = false;

    TxFlowHoverCard *m_card = nullptr;
};

// A legend entry: a small swatch in a band's color, then its text.
class TxLegend : public QWidget {
    Q_OBJECT

public:
    // Inputs and change share the self color.
    enum class Swatch { Self, Recipient, Fee };

    TxLegend(Swatch swatch, const QString &text, QWidget *parent = nullptr);
    void setText(const QString &text);
    [[nodiscard]] static auto swatchFor(TxFlow::OutputKind kind) -> Swatch;

private:
    Label *m_text = nullptr;
};

// The inset card around a TxFlow with its footer: input count, legend, output count.
// Shared by the Send panel and the transaction summary.
class TxFlowCard : public Card {
    Q_OBJECT

public:
    explicit TxFlowCard(QWidget *parent = nullptr);
    void setInputs(const QList<TxFlow::InputItem> &inputs);
    void setOutputs(const QList<TxFlow::OutputItem> &outputs);
    void setDetail(bool detail);

protected:
    void updateFooter();

private:
    TxFlow *m_flow = nullptr;
    QWidget *m_footer = nullptr;
    Label *m_inputs_count = nullptr;
    Label *m_outputs_count = nullptr;
    TxLegend *m_recipient = nullptr;
    TxLegend *m_change = nullptr;
    TxLegend *m_fee = nullptr;
};

} // namespace catalog
