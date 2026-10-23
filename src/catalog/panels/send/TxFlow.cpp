#include "TxFlow.h"

#include "catalog/display/Amount.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"
#include "theme/Theme.h"

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsDropShadowEffect>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QShowEvent>
#include <Qontrol>
#include <common.h>

namespace catalog {

namespace {

// TxFlow.jsx: a 300 wide canvas, drawn at 1:1 and centered, scaled down only when the
// widget is narrower. Detail mode draws on the widget's own width.
constexpr double CANVAS_W = 300.0;
constexpr int CANVAS_H = 188;
constexpr int DETAIL_CANVAS_H = 220;
constexpr double PAD_Y = 14.0;

constexpr double X_IN_STUB = 4.0;      // xInStub
constexpr double FLOW_INSET = 24.0;    // xFlowL, and xFlowR from the right edge
constexpr double DETAIL_INSET = 30.0;  // wider caps holding each band's index
constexpr double TRUNK_L_FRAC = 0.44;  // xTrunkL
constexpr double TRUNK_R_FRAC = 0.56;  // xTrunkR
constexpr double OUT_STUB_INSET = 4.0; // xOutStub from the right edge

constexpr double TRUNK_FRAC = 0.84; // trunk height fraction of usable height
constexpr double NODE_GAP = 9.0;    // gap between node bands
constexpr double BAND_MIN = 5.0;    // minimum band thickness (min)
constexpr double CAP_INSET = 2.0;   // cap rect inset from flow column
constexpr double CAP_RADIUS = 2.0;  // cap rounded-rect radius (rx)
constexpr double LABEL_INSET = 8.0; // label inset from the flow column

constexpr int KIND_FONT_PX = 9;           // output kind labels
constexpr double KIND_TRACKING_PX = 0.54; // design 0.06em at 9px
constexpr double KIND_BASELINE = 3.0;     // text baseline below the band middle
constexpr int DETAIL_FONT_PX = 11;        // detail indexes and ids
constexpr double DETAIL_BASELINE = 4.0;   // text baseline below the band middle
constexpr double DETAIL_CAP_PADDING = 2.0;
constexpr int LABEL_MAX_CHARS = 10; // recipient label truncation

constexpr double RIBBON_HOVER_ALPHA = 0.98;
constexpr double RIBBON_IN_BASE = 0.34; // input ribbon base alpha
constexpr double RIBBON_IN_STEP = 0.07; // input ribbon per-index alpha step
constexpr int RIBBON_ALPHA_CYCLE = 3;   // input ribbon alpha repeats every N bands
constexpr double TRUNK_ALPHA = 0.95;
constexpr double RIBBON_OUT_ALPHA = 0.62;

constexpr int CARD_WIDTH = 196;
constexpr int CARD_OFFSET = 14; // cursor-to-card offset
constexpr int CARD_MARGIN = 4;  // clamp margin to the diagram and window edges
constexpr int CARD_RADIUS = 8;
constexpr int CARD_PAD_H = 11;
constexpr int CARD_PAD_V = 9;
constexpr int CARD_BLUR = 24;
constexpr int CARD_SHADOW_DY = 8;
constexpr int CARD_SHADOW_ALPHA = 89;
constexpr int CARD_TITLE_PX = 13;
constexpr int CARD_FIAT_PX = 13;
constexpr int CARD_ADDR_PX = 11;

// TxFlowEmpty: a 116x60 sketch above a muted line of text at most 200 wide.
constexpr int EMPTY_ART_W = 116;
constexpr int EMPTY_ART_H = 60;
constexpr int EMPTY_GAP = 10;
constexpr int EMPTY_TEXT_W = 200;
constexpr int EMPTY_TEXT_PX = 13;
constexpr double EMPTY_STUB_ALPHA = 0.4;
constexpr double EMPTY_TRUNK_ALPHA = 0.55;
constexpr double EMPTY_DASH_ALPHA = 0.3;
constexpr double EMPTY_DASH_WIDTH = 1.5;
constexpr double EMPTY_DASH = 3.0;

constexpr int FOOTER_PAD_H = 6; // design footer padding 6px 6px 2px
constexpr int FOOTER_PAD_BOTTOM = 2;
constexpr int FOOTER_EYEBROW_PX = 10;
constexpr int LEGEND_GAP = 12;
constexpr int LEGEND_SWATCH = 8;
constexpr double LEGEND_SWATCH_RADIUS = 2.0;

struct Band {
    double top = 0.0;
    double bot = 0.0;
};

// Proportional band layout: reserve BAND_MIN per band so tiny amounts stay
// visible, then share leftover space proportionally to weight.
auto txBands(const QList<double> &weights, double top, double bottom, double gap,
             double minimum = BAND_MIN) -> QList<Band> {
    double total = 0.0;
    for (double w : weights) {
        total += w;
    }
    if (total <= 0.0) {
        total = 1.0;
    }
    int n = static_cast<int>(weights.size());
    double avail = (bottom - top) - gap * std::max(0, n - 1);
    double per = n > 0 ? std::min(minimum, avail / n) : avail;
    double leftover = std::max(0.0, avail - per * n);
    QList<Band> bands;
    double y = top;
    for (double w : weights) {
        double h = per + leftover * (w / total);
        bands.append(Band{.top = y, .bot = y + h});
        y += h + gap;
    }
    return bands;
}

auto ribbonPath(double x0, double a0, double a1, double x1, double b0, double b1) -> QPainterPath {
    double mx = (x0 + x1) / 2.0;
    QPainterPath path;
    path.moveTo(x0, a0);
    path.cubicTo(mx, a0, mx, b0, x1, b0);
    path.lineTo(x1, b1);
    path.cubicTo(mx, b1, mx, a1, x0, a1);
    path.closeSubpath();
    return path;
}

// The x anchors of the diagram on a canvas `w` wide.
struct Columns {
    double flowL = 0.0;
    double trunkL = 0.0;
    double trunkR = 0.0;
    double flowR = 0.0;
    double outStub = 0.0;
};

auto columns(double w, bool detail) -> Columns {
    double inset = detail ? DETAIL_INSET : FLOW_INSET;
    return {.flowL = inset,
            .trunkL = w * TRUNK_L_FRAC,
            .trunkR = w * TRUNK_R_FRAC,
            .flowR = w - inset,
            .outStub = w - OUT_STUB_INSET};
}

auto kindFill(const Palette &p, TxFlow::OutputKind kind) -> QColor {
    switch (kind) {
    case TxFlow::OutputKind::Change:
        return p.txSelf;
    case TxFlow::OutputKind::Fee:
        return p.error;
    case TxFlow::OutputKind::Recipient:
        return p.txWarning;
    }
    return p.txWarning;
}

auto kindLabelColor(const Palette &p, TxFlow::OutputKind kind) -> QColor {
    switch (kind) {
    case TxFlow::OutputKind::Change:
        return p.txSelfLabel;
    case TxFlow::OutputKind::Fee:
        return p.txFeeLabel;
    case TxFlow::OutputKind::Recipient:
        return p.txRecipientLabel;
    }
    return p.txRecipientLabel;
}

auto kindLabelText(const TxFlow::OutputItem &item) -> QString {
    switch (item.kind) {
    case TxFlow::OutputKind::Change:
        return TR("txflow-change").toUpper();
    case TxFlow::OutputKind::Fee:
        return TR("txflow-fee").toUpper();
    case TxFlow::OutputKind::Recipient:
        return (item.label.isEmpty() ? TR("txflow-recipient") : item.label.left(LABEL_MAX_CHARS))
            .toUpper();
    }
    return TR("txflow-recipient").toUpper();
}

enum class Anchor { Start, Middle, End };

// Draws `text` on `baseline` anchored at `x` as SVG text-anchor does.
void drawAnchored(QPainter &painter, double x, double baseline, Anchor anchor,
                  const QString &text) {
    double advance = QFontMetricsF(painter.font()).horizontalAdvance(text);
    double left = x;
    switch (anchor) {
    case Anchor::Start:
        break;
    case Anchor::Middle:
        left = x - advance / 2.0;
        break;
    case Anchor::End:
        left = x - advance;
        break;
    }
    painter.drawText(QPointF(left, baseline), text);
}

auto monoFont(int px, double tracking_px = 0.0) -> QFont {
    QFont f(font::MONO);
    f.setPixelSize(px);
    f.setWeight(font::DEMIBOLD);
    if (tracking_px > 0.0) {
        f.setLetterSpacing(QFont::AbsoluteSpacing, tracking_px);
    }
    return f;
}

auto footerEyebrow(QWidget *parent) -> Label * {
    auto *label = new Label(LabelRole::Eyebrow, parent);
    auto eyebrowFont = label->font();
    eyebrowFont.setPixelSize(FOOTER_EYEBROW_PX);
    eyebrowFont.setWeight(font::DEMIBOLD);
    label->setFont(eyebrowFont);
    return label;
}

auto countText(qsizetype count, const QString &one, const QString &many) -> QString {
    return count == 1 ? one : many.arg(count);
}

// Tiny rounded square in a band color; resolves the palette at paint time so it
// tracks theme changes.
class LegendSwatch : public QWidget {
public:
    LegendSwatch(TxLegend::Swatch swatch, QWidget *parent) : QWidget(parent), m_swatch(swatch) {
        setFixedSize(LEGEND_SWATCH, LEGEND_SWATCH);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto &p = Theme::get()->palette();
        QColor fill = p.txSelf;
        switch (m_swatch) {
        case TxLegend::Swatch::Self:
            break;
        case TxLegend::Swatch::Recipient:
            fill = p.warning;
            break;
        case TxLegend::Swatch::Fee:
            fill = p.error;
            break;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(rect(), LEGEND_SWATCH_RADIUS, LEGEND_SWATCH_RADIUS);
    }

private:
    TxLegend::Swatch m_swatch;
};

} // namespace

// Floating detail card shown on hover; transparent to the mouse so it never
// steals hover events from the ribbons underneath.
class TxFlowHoverCard : public QWidget {
    Q_OBJECT

public:
    explicit TxFlowHoverCard(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedWidth(CARD_WIDTH);

        m_title = new Label(LabelRole::Body);
        QFont titleFont = m_title->font();
        titleFont.setPixelSize(CARD_TITLE_PX);
        titleFont.setWeight(font::DEMIBOLD);
        m_title->setFont(titleFont);

        m_fiat = new Label(LabelRole::Mono);
        m_fiat->setTone(LabelTone::Secondary);
        QFont fiatFont = m_fiat->font();
        fiatFont.setPixelSize(CARD_FIAT_PX);
        m_fiat->setFont(fiatFont);

        m_addr = new Label(LabelRole::Mono);
        m_addr->setFixedWidth(CARD_WIDTH - 2 * CARD_PAD_H);
        m_addr->setTone(LabelTone::Muted);
        QFont addrFont = m_addr->font();
        addrFont.setPixelSize(CARD_ADDR_PX);
        m_addr->setFont(addrFont);

        (new qontrol::Column)
            ->margins(CARD_PAD_H, CARD_PAD_V, CARD_PAD_H, CARD_PAD_V)
            ->push(m_title)
            ->push(m_fiat)
            ->push(m_addr)
            ->into(this);

        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(CARD_BLUR);
        shadow->setOffset(0, CARD_SHADOW_DY);
        shadow->setColor(QColor(0, 0, 0, CARD_SHADOW_ALPHA));
        setGraphicsEffect(shadow);
    }

    void setData(const QString &title, const QString &fiat, const QString &addr) {
        m_title->setText(title);
        m_fiat->setText(fiat);
        m_fiat->setVisible(!fiat.isEmpty());
        m_addr->setTextWrapAnywhere(addr);
        m_addr->ensurePolished();
        m_addr->setMinimumHeight(m_addr->heightForWidth(m_addr->width()));
        m_addr->setVisible(!addr.isEmpty());
        m_addr->parentWidget()->layout()->activate();
        layout()->activate();
        adjustSize();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto &p = Theme::get()->palette();
        QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setBrush(p.surface);
        painter.setPen(QPen(p.border, 1));
        painter.drawRoundedRect(box, CARD_RADIUS, CARD_RADIUS);
    }

private:
    Label *m_title = nullptr;
    Label *m_fiat = nullptr;
    Label *m_addr = nullptr;
};

TxFlow::TxFlow(QWidget *parent) : QWidget(parent) {
    setProperty("class", "tx-flow");
    setMouseTracking(true);
    // Fixed-height diagram (the design's fixed height); pin it so a constrained
    // column (single-column layout) cannot collapse it to nothing.
    setFixedHeight(canvasHeight());
    // A layout-free layer keeps card layout and visibility changes out of the modal's refit.
    m_card_layer = new QWidget(this);
    m_card_layer->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_card = new TxFlowHoverCard(m_card_layer);
    m_card->hide();
    connect(AmountDisplay::get(), &AmountDisplay::changed, this, &TxFlow::onAmountModeChanged,
            qontrol::UNIQUE);
}

TxFlow::~TxFlow() {
    // The layer is reparented to the top-level window, so it is not freed
    // with this widget's children; delete it explicitly.
    delete m_card_layer;
}

void TxFlow::setInputs(const QList<InputItem> &inputs) {
    m_given_inputs = inputs;
    applyItems();
}

void TxFlow::setOutputs(const QList<OutputItem> &outputs) {
    m_given_outputs = outputs;
    applyItems();
}

void TxFlow::setDetail(bool detail) {
    m_detail = detail;
    setFixedHeight(canvasHeight());
    applyItems();
}

void TxFlow::applyItems() {
    m_inputs.clear();
    for (const auto &item : m_given_inputs) {
        if (m_detail || item.weight > 0.0) {
            m_inputs.append(item);
        }
    }
    m_outputs.clear();
    for (const auto &item : m_given_outputs) {
        if (m_detail || item.weight > 0.0) {
            m_outputs.append(item);
        }
    }
    clearHover();
}

auto TxFlow::inputs() const -> const QList<InputItem> & {
    return m_inputs;
}

auto TxFlow::outputs() const -> const QList<OutputItem> & {
    return m_outputs;
}

void TxFlow::clearHover() {
    m_hover_index = -1;
    m_card->hide();
    update();
}

auto TxFlow::canvasHeight() const -> int {
    return m_detail ? DETAIL_CANVAS_H : CANVAS_H;
}

auto TxFlow::canvasMap() const -> CanvasMap {
    double canvasW = m_detail ? std::max(1, width()) : CANVAS_W;
    double scale = std::min(1.0, width() / canvasW);
    return {.width = canvasW,
            .scale = scale,
            .dx = (width() - canvasW * scale) / 2.0,
            .dy = (height() - canvasHeight() * scale) / 2.0};
}

void TxFlow::recomputeGeometry(double canvas_width) {
    m_input_ribbons.clear();
    m_output_ribbons.clear();
    m_input_caps.clear();
    m_output_caps.clear();
    if (m_inputs.isEmpty() || m_outputs.isEmpty()) {
        return;
    }

    Columns x = columns(canvas_width, m_detail);
    double h = canvasHeight();
    double trunkH = (h - 2.0 * PAD_Y) * TRUNK_FRAC;
    m_trunk_top = (h - trunkH) / 2.0;
    m_trunk_bot = m_trunk_top + trunkH;

    QList<double> inW;
    for (const auto &item : m_inputs) {
        inW.append(item.weight);
    }
    QList<double> outW;
    for (const auto &item : m_outputs) {
        outW.append(item.weight);
    }

    double nodeMinimum = m_detail ? DETAIL_FONT_PX + 2 * DETAIL_CAP_PADDING : BAND_MIN;
    auto inNode = txBands(inW, PAD_Y, h - PAD_Y, NODE_GAP, nodeMinimum);
    auto inTrunk = txBands(inW, m_trunk_top, m_trunk_bot, 0.0);
    auto outNode = txBands(outW, PAD_Y, h - PAD_Y, NODE_GAP, nodeMinimum);
    auto outTrunk = txBands(outW, m_trunk_top, m_trunk_bot, 0.0);

    for (int i = 0; i < m_inputs.size(); ++i) {
        m_input_ribbons.append(ribbonPath(x.flowL, inNode[i].top, inNode[i].bot, x.trunkL,
                                          inTrunk[i].top, inTrunk[i].bot));
        m_input_caps.append(QRectF(X_IN_STUB, inNode[i].top, x.flowL - X_IN_STUB - CAP_INSET,
                                   inNode[i].bot - inNode[i].top));
    }
    for (int j = 0; j < m_outputs.size(); ++j) {
        m_output_ribbons.append(ribbonPath(x.trunkR, outTrunk[j].top, outTrunk[j].bot, x.flowR,
                                           outNode[j].top, outNode[j].bot));
        m_output_caps.append(QRectF(x.flowR + CAP_INSET, outNode[j].top,
                                    x.outStub - x.flowR - CAP_INSET,
                                    outNode[j].bot - outNode[j].top));
    }
}

void TxFlow::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (m_inputs.isEmpty() || m_outputs.isEmpty()) {
        m_card->hide();
        // The payment prompt belongs to Send: a transaction summary has no empty state.
        if (!m_detail) {
            paintEmpty(painter);
        }
        return;
    }

    CanvasMap map = canvasMap();
    recomputeGeometry(map.width);
    Columns x = columns(map.width, m_detail);
    const auto &p = Theme::get()->palette();

    painter.translate(map.dx, map.dy);
    painter.scale(map.scale, map.scale);
    painter.setPen(Qt::NoPen);

    for (int i = 0; i < m_input_ribbons.size(); ++i) {
        bool hot = m_hover_is_input && m_hover_index == i;
        double alpha =
            hot ? RIBBON_HOVER_ALPHA : RIBBON_IN_BASE + RIBBON_IN_STEP * (i % RIBBON_ALPHA_CYCLE);
        painter.setBrush(withAlpha(p.txSelf, alpha));
        painter.drawPath(m_input_ribbons[i]);
    }

    painter.setBrush(withAlpha(p.txSelf, TRUNK_ALPHA));
    painter.drawRect(QRectF(x.trunkL, m_trunk_top, x.trunkR - x.trunkL, m_trunk_bot - m_trunk_top));

    for (int j = 0; j < m_output_ribbons.size(); ++j) {
        bool hot = !m_hover_is_input && m_hover_index == j;
        painter.setBrush(
            withAlpha(kindFill(p, m_outputs[j].kind), hot ? RIBBON_HOVER_ALPHA : RIBBON_OUT_ALPHA));
        painter.drawPath(m_output_ribbons[j]);
    }

    painter.setBrush(p.txSelf);
    for (const auto &cap : m_input_caps) {
        painter.drawRoundedRect(cap, CAP_RADIUS, CAP_RADIUS);
    }
    for (int j = 0; j < m_output_caps.size(); ++j) {
        painter.setBrush(kindFill(p, m_outputs[j].kind));
        painter.drawRoundedRect(m_output_caps[j], CAP_RADIUS, CAP_RADIUS);
    }

    paintLabels(painter, map.width);
}

void TxFlow::paintLabels(QPainter &painter, double canvas_width) {
    const auto &p = Theme::get()->palette();
    Columns x = columns(canvas_width, m_detail);

    if (!m_detail) {
        painter.setFont(monoFont(KIND_FONT_PX, KIND_TRACKING_PX));
        for (int j = 0; j < m_outputs.size(); ++j) {
            painter.setPen(kindLabelColor(p, m_outputs[j].kind));
            drawAnchored(painter, x.flowR - LABEL_INSET,
                         m_output_caps[j].center().y() + KIND_BASELINE, Anchor::End,
                         kindLabelText(m_outputs[j]));
        }
        return;
    }

    painter.setFont(monoFont(DETAIL_FONT_PX));
    for (int i = 0; i < m_inputs.size(); ++i) {
        double baseline = m_input_caps[i].center().y() + DETAIL_BASELINE;
        painter.setPen(p.txSelfLabel);
        drawAnchored(painter, x.flowL + LABEL_INSET, baseline, Anchor::Start, m_inputs[i].ref);
        painter.setPen(p.bg);
        drawAnchored(painter, m_input_caps[i].center().x(), baseline, Anchor::Middle,
                     m_inputs[i].index);
    }
    for (int j = 0; j < m_outputs.size(); ++j) {
        const auto &item = m_outputs[j];
        double baseline = m_output_caps[j].center().y() + DETAIL_BASELINE;
        bool fee = item.kind == OutputKind::Fee;
        painter.setPen(kindLabelColor(p, item.kind));
        drawAnchored(painter, x.flowR - LABEL_INSET, baseline, Anchor::End,
                     fee ? TR("txflow-legend-fee") : item.ref);
        if (!fee) {
            painter.setPen(item.kind == OutputKind::Change ? p.bg : p.text);
            drawAnchored(painter, m_output_caps[j].center().x(), baseline, Anchor::Middle,
                         item.index);
        }
    }
}

void TxFlow::paintEmpty(QPainter &painter) {
    const auto &p = Theme::get()->palette();
    QFont textFont = font();
    textFont.setPixelSize(EMPTY_TEXT_PX);
    QRect textRect = QFontMetrics(textFont).boundingRect(QRect(0, 0, EMPTY_TEXT_W, height()),
                                                         Qt::AlignHCenter | Qt::TextWordWrap,
                                                         TR("txflow-empty"));
    int top = (height() - (EMPTY_ART_H + EMPTY_GAP + textRect.height())) / 2;

    painter.save();
    painter.translate((width() - EMPTY_ART_W) / 2.0, top);
    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(p.textMuted, EMPTY_STUB_ALPHA));
    painter.drawRoundedRect(QRectF(3, 14, 9, 13), CAP_RADIUS, CAP_RADIUS);
    painter.drawRoundedRect(QRectF(3, 33, 9, 13), CAP_RADIUS, CAP_RADIUS);
    painter.drawRoundedRect(QRectF(104, 20, 9, 20), CAP_RADIUS, CAP_RADIUS);
    painter.setBrush(withAlpha(p.textMuted, EMPTY_TRUNK_ALPHA));
    painter.drawRoundedRect(QRectF(51, 18, 14, 24), CAP_RADIUS, CAP_RADIUS);

    QPen dash(withAlpha(p.textMuted, EMPTY_DASH_ALPHA), EMPTY_DASH_WIDTH);
    // Qt counts dash lengths in pen widths.
    dash.setDashPattern({EMPTY_DASH / EMPTY_DASH_WIDTH, EMPTY_DASH / EMPTY_DASH_WIDTH});
    painter.setPen(dash);
    painter.setBrush(Qt::NoBrush);
    QPainterPath connectors;
    connectors.moveTo(12, 20);
    connectors.cubicTo(32, 20, 32, 24, 51, 24);
    connectors.moveTo(12, 40);
    connectors.cubicTo(32, 40, 32, 36, 51, 36);
    connectors.moveTo(65, 30);
    connectors.lineTo(104, 30);
    painter.drawPath(connectors);
    painter.restore();

    painter.setFont(textFont);
    painter.setPen(p.textMuted);
    painter.drawText(QRect((width() - EMPTY_TEXT_W) / 2, top + EMPTY_ART_H + EMPTY_GAP,
                           EMPTY_TEXT_W, textRect.height()),
                     Qt::AlignHCenter | Qt::TextWordWrap, TR("txflow-empty"));
}

auto TxFlow::sizeHint() const -> QSize {
    return {static_cast<int>(CANVAS_W), canvasHeight()};
}

auto TxFlow::hitTest(const QPointF &pos) const -> HoverHit {
    CanvasMap map = canvasMap();
    QPointF vpos((pos.x() - map.dx) / map.scale, (pos.y() - map.dy) / map.scale);

    for (int i = 0; i < m_input_ribbons.size(); ++i) {
        if (m_input_ribbons[i].contains(vpos) || m_input_caps[i].contains(vpos)) {
            return {.index = i, .is_input = true};
        }
    }
    for (int j = 0; j < m_output_ribbons.size(); ++j) {
        if (m_output_ribbons[j].contains(vpos) || m_output_caps[j].contains(vpos)) {
            return {.index = j, .is_input = false};
        }
    }
    return {};
}

void TxFlow::positionCard(const QPointF &pos) {
    // Float the card over the whole window, so no container clips it.
    QWidget *top = window();
    // TxFlow.jsx: right of the cursor, or left when it would leave the diagram, kept
    // within the diagram's width.
    int x = static_cast<int>(pos.x());
    int cardL = x + CARD_OFFSET;
    if (cardL + CARD_WIDTH > width()) {
        cardL = x - CARD_OFFSET - CARD_WIDTH;
    }
    cardL =
        std::clamp(cardL, CARD_MARGIN, std::max(CARD_MARGIN, width() - CARD_WIDTH - CARD_MARGIN));
    int cardT = std::max(CARD_MARGIN, static_cast<int>(pos.y()) - CARD_OFFSET);

    m_card->move(mapTo(top, QPoint(cardL, cardT)));
    fitCardToWindow();
    m_card_layer->raise();
    m_card->show();
}

void TxFlow::fitCardToWindow() {
    auto *top = m_card_layer->parentWidget();
    m_card_layer->setGeometry(top->rect());
    int left = std::clamp(m_card->x(), CARD_MARGIN,
                          std::max(CARD_MARGIN, top->width() - m_card->width() - CARD_MARGIN));
    int topY = std::clamp(m_card->y(), CARD_MARGIN,
                          std::max(CARD_MARGIN, top->height() - m_card->height() - CARD_MARGIN));
    m_card->move(left, topY);
}

void TxFlow::mouseMoveEvent(QMouseEvent *event) {
    if (m_inputs.isEmpty() || m_outputs.isEmpty() || m_input_ribbons.isEmpty()) {
        return;
    }

    HoverHit hit = hitTest(event->position());

    if (hit.index < 0) {
        if (m_hover_index >= 0) {
            clearHover();
        }
        return;
    }

    if (m_hover_index != hit.index || m_hover_is_input != hit.is_input) {
        m_hover_index = hit.index;
        m_hover_is_input = hit.is_input;
        updateCard();
        update();
    }

    positionCard(event->position());
}

void TxFlow::updateCard() {
    if (m_hover_index < 0) {
        return;
    }
    if (m_hover_is_input) {
        const auto &item = m_inputs[m_hover_index];
        m_card->setData(item.title, cardValue(item.value_known, item.fiat), item.addr);
    } else {
        const auto &item = m_outputs[m_hover_index];
        m_card->setData(item.title, cardValue(item.value_known, item.fiat), item.addr);
    }
}

auto TxFlow::cardValue(bool value_known, const QString &fiat) const -> QString {
    if (!value_known) {
        return TR("tx-unknown");
    }
    // Send keeps its amounts fixed; the summary follows the app-wide hidden mode.
    if (m_detail && AmountDisplay::get()->mode() == AmountMode::Hidden) {
        return {};
    }
    return fiat;
}

void TxFlow::leaveEvent(QEvent *event) {
    if (m_hover_index >= 0) {
        clearHover();
    }
    QWidget::leaveEvent(event);
}

void TxFlow::hideEvent(QHideEvent *event) {
    m_hover_index = -1;
    m_card->hide();
    // Keep the hidden layer with the flow if it moves to another window.
    m_card_layer->parentWidget()->removeEventFilter(this);
    m_card_layer->setParent(this);
    QWidget::hideEvent(event);
}

void TxFlow::showEvent(QShowEvent *event) {
    auto *top = window();
    auto *oldTop = m_card_layer->parentWidget();
    if (oldTop != top) {
        oldTop->removeEventFilter(this);
        m_card_layer->setParent(top);
    }
    top->installEventFilter(this);
    fitCardToWindow();
    m_card_layer->show();
    QWidget::showEvent(event);
}

auto TxFlow::eventFilter(QObject *object, QEvent *event) -> bool {
    if (event->type() == QEvent::Resize && object == m_card_layer->parentWidget()) {
        fitCardToWindow();
    }
    return QWidget::eventFilter(object, event);
}

void TxFlow::onAmountModeChanged() {
    updateCard();
}

TxLegend::TxLegend(Swatch swatch, const QString &text, QWidget *parent) : QWidget(parent) {
    m_text = new Label(text, LabelRole::Body, this);
    (new qontrol::Row)
        ->spacing(resolve(Spacing::XS))
        ->push(new LegendSwatch(swatch, this), 0, Qt::AlignVCenter)
        ->push(m_text, 0, Qt::AlignVCenter)
        ->into(this);
}

void TxLegend::setText(const QString &text) {
    m_text->setText(text);
}

auto TxLegend::swatchFor(TxFlow::OutputKind kind) -> Swatch {
    switch (kind) {
    case TxFlow::OutputKind::Recipient:
        return Swatch::Recipient;
    case TxFlow::OutputKind::Change:
        return Swatch::Self;
    case TxFlow::OutputKind::Fee:
        return Swatch::Fee;
    }
    return Swatch::Recipient;
}

TxFlowCard::TxFlowCard(QWidget *parent) : Card(Role::Inset, parent) {
    setPad(Pad::S);
    m_flow = new TxFlow(this);

    m_inputs_count = footerEyebrow(this);
    m_outputs_count = footerEyebrow(this);
    m_recipient = new TxLegend(TxLegend::Swatch::Recipient, TR("txflow-legend-recipient"), this);
    m_change = new TxLegend(TxLegend::Swatch::Self, TR("txflow-legend-change"), this);
    m_fee = new TxLegend(TxLegend::Swatch::Fee, TR("txflow-legend-fee"), this);
    auto *legend =
        (new qontrol::Row)->spacing(LEGEND_GAP)->push(m_recipient)->push(m_change)->push(m_fee);
    m_footer = (new qontrol::Row)
                   ->margins(FOOTER_PAD_H, FOOTER_PAD_H, FOOTER_PAD_H, FOOTER_PAD_BOTTOM)
                   ->push(m_inputs_count, 0, Qt::AlignVCenter)
                   ->pushSpacer()
                   ->push(legend, 0, Qt::AlignVCenter)
                   ->pushSpacer()
                   ->push(m_outputs_count, 0, Qt::AlignVCenter);

    auto *column = (new qontrol::Column)->push(m_flow)->push(m_footer);
    setContent(column);
    updateFooter();
}

void TxFlowCard::setInputs(const QList<TxFlow::InputItem> &inputs) {
    m_flow->setInputs(inputs);
    updateFooter();
}

void TxFlowCard::setOutputs(const QList<TxFlow::OutputItem> &outputs) {
    m_flow->setOutputs(outputs);
    updateFooter();
}

void TxFlowCard::setDetail(bool detail) {
    m_flow->setDetail(detail);
    updateFooter();
}

void TxFlowCard::updateFooter() {
    const auto &inputs = m_flow->inputs();
    const auto &outputs = m_flow->outputs();
    // The footer describes a drawn flow: none while the diagram shows its empty state.
    m_footer->setVisible(!inputs.isEmpty() && !outputs.isEmpty());
    m_inputs_count->setText(
        countText(inputs.size(), TR("txflow-input-count-one"), TR("txflow-inputs-count")));
    m_outputs_count->setText(
        countText(outputs.size(), TR("txflow-output-count-one"), TR("txflow-outputs-count")));

    m_recipient->setVisible(false);
    m_change->setVisible(false);
    m_fee->setVisible(false);
    for (const auto &output : outputs) {
        switch (output.kind) {
        case TxFlow::OutputKind::Recipient:
            m_recipient->setVisible(true);
            break;
        case TxFlow::OutputKind::Change:
            m_change->setVisible(true);
            break;
        case TxFlow::OutputKind::Fee:
            m_fee->setVisible(true);
            break;
        }
    }
}

} // namespace catalog

#include "TxFlow.moc"
