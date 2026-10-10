#include "TxFlow.h"

#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"
#include "theme/Theme.h"

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <Qontrol>

namespace catalog {

namespace {

// Virtual canvas: TxFlow.jsx uses a 300x188 viewBox. Width maps to a fraction of
// the widget width (GRAPH_WIDTH_FRAC); height is a fixed value (taller than 188).
constexpr double CANVAS_W = 300.0; // virtual width (design viewBox is 300x188)
constexpr int CANVAS_H = 250;      // rendered height (taller than the design's 188)
constexpr double PAD_Y = 14.0;     // vertical padding (padY)

// Virtual X anchors (TxFlow.jsx).
constexpr double X_IN_STUB = 4.0;               // xInStub
constexpr double X_FLOW_L = 24.0;               // xFlowL
constexpr double X_TRUNK_L = CANVAS_W * 0.44; // xTrunkL
constexpr double X_TRUNK_R = CANVAS_W * 0.56; // xTrunkR
constexpr double X_FLOW_R = CANVAS_W - 24.0;  // xFlowR
constexpr double X_OUT_STUB = CANVAS_W - 4.0; // xOutStub

constexpr double TRUNK_FRAC = 0.84; // trunk height fraction of usable height
constexpr double NODE_GAP = 9.0;    // gap between node bands
constexpr double BAND_MIN = 5.0;    // minimum band thickness (min)
constexpr double CAP_INSET = 2.0;   // cap rect inset from flow column
constexpr double CAP_RADIUS = 2.0;  // cap rounded-rect radius (rx)
constexpr double LABEL_INSET = 8.0; // label right inset from xFlowR

// The drawn diagram occupies this fraction of the widget width, centered.
constexpr double GRAPH_WIDTH_FRAC = 0.7;

constexpr int LABEL_FONT_PX = 9;    // on-ribbon label font size
constexpr int LABEL_SPACING = 106;  // letter spacing percentage
constexpr int LABEL_MAX_CHARS = 10; // recipient label truncation

constexpr double RIBBON_HOVER_ALPHA = 0.98;
constexpr double RIBBON_IN_BASE = 0.34;     // input ribbon base alpha
constexpr double RIBBON_IN_STEP = 0.07;     // input ribbon per-index alpha step
constexpr int RIBBON_ALPHA_CYCLE = 3;       // input ribbon alpha repeats every N bands
constexpr double TRUNK_ALPHA = 0.95;
constexpr double RIBBON_OUT_ALPHA = 0.62;

constexpr int CARD_WIDTH = 196;
constexpr int CARD_OFFSET = 14; // cursor-to-card offset
constexpr int CARD_MARGIN = 4;  // clamp margin to widget edges
constexpr int CARD_RADIUS = 8;
constexpr int CARD_PAD_H = 11;
constexpr int CARD_PAD_V = 9;
constexpr int CARD_SPACING = 3;
constexpr int CARD_BLUR = 24;
constexpr int CARD_SHADOW_DY = 8;
constexpr int CARD_SHADOW_ALPHA = 89;
constexpr int CARD_TITLE_PX = 12;
constexpr int CARD_USD_PX = 13;
constexpr int CARD_ADDR_PX = 11;
constexpr int ADDR_FULL = 30; // addr length above which to shorten
constexpr int ADDR_HEAD = 8;  // leading chars kept
constexpr int ADDR_TAIL = 8;  // trailing chars kept

// Empty-state placeholder geometry (centered preview), all relative to the
// canvas center (cx, cy).
constexpr double EMPTY_ALPHA = 0.4;
constexpr double EMPTY_DASH_ALPHA = 0.3;
constexpr double EMPTY_TRUNK_ALPHA = 0.55; // muted trunk fill alpha

constexpr double EMPTY_STUB_X = -58.0;     // left stubs x offset from center
constexpr double EMPTY_STUB_W = 9.0;       // side stub width
constexpr double EMPTY_STUB_H = 13.0;      // input stub height
constexpr double EMPTY_STUB_TOP_Y = -20.0; // top input stub y offset
constexpr double EMPTY_STUB_BOT_Y = 7.0;   // bottom input stub y offset

constexpr double EMPTY_TRUNK_X = -7.0; // trunk x offset from center
constexpr double EMPTY_TRUNK_Y = -12.0; // trunk top y offset
constexpr double EMPTY_TRUNK_W = 14.0;  // trunk width
constexpr double EMPTY_TRUNK_H = 24.0;  // trunk height

constexpr double EMPTY_OUT_X = 46.0;   // right stub x offset from center
constexpr double EMPTY_OUT_Y = -10.0;  // right stub top y offset
constexpr double EMPTY_OUT_H = 20.0;   // right stub height

constexpr double EMPTY_DASH_WIDTH = 1.5; // dashed connector pen width
constexpr double EMPTY_DASH_ON = 3.0;    // dash pattern: on length
constexpr double EMPTY_DASH_OFF = 3.0;   // dash pattern: off length

constexpr double EMPTY_CONN_IN_X = -49.0;     // input connector start x offset
constexpr double EMPTY_CONN_TOP_Y0 = -8.0;    // top connector start y offset
constexpr double EMPTY_CONN_TOP_Y1 = -4.0;    // top connector end y offset
constexpr double EMPTY_CONN_BOT_Y0 = 14.0;    // bottom connector start y offset
constexpr double EMPTY_CONN_BOT_Y1 = 8.0;     // bottom connector end y offset
constexpr double EMPTY_CONN_OUT_X0 = 7.0;     // output connector start x offset
constexpr double EMPTY_CONN_OUT_X1 = 46.0;    // output connector end x offset

constexpr double EMPTY_TEXT_TOP = 24.0; // body text top offset below center

struct Band {
    double top = 0.0;
    double bot = 0.0;
};

// Proportional band layout: reserve BAND_MIN per band so tiny amounts stay
// visible, then share leftover space proportionally to weight.
auto txBands(const QList<double> &weights, double top, double bottom, double gap) -> QList<Band> {
    double total = 0.0;
    for (double w : weights) {
        total += w;
    }
    if (total <= 0.0) {
        total = 1.0;
    }
    int n = static_cast<int>(weights.size());
    double avail = (bottom - top) - gap * std::max(0, n - 1);
    double per = n > 0 ? std::min(BAND_MIN, avail / n) : avail;
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

// Horizontal mapping from the virtual canvas to device pixels: the diagram is
// scaled to GRAPH_WIDTH_FRAC of the widget width and centered.
struct GraphX {
    double scale = 1.0;
    double offset = 0.0;
};

auto graphX(int widget_width) -> GraphX {
    double drawW = widget_width * GRAPH_WIDTH_FRAC;
    return {.scale = drawW / CANVAS_W, .offset = (widget_width - drawW) / 2.0};
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

auto txMidAddr(const QString &s) -> QString {
    if (s.size() > ADDR_FULL) {
        return s.left(ADDR_HEAD) + QString::fromUtf8("… ") + s.right(ADDR_TAIL);
    }
    return s;
}

} // namespace

// Floating detail card shown on hover; transparent to the mouse so it never
// steals hover events from the ribbons underneath.
class TxFlowCard : public QWidget {
    Q_OBJECT

public:
    explicit TxFlowCard(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedWidth(CARD_WIDTH);

        m_title = new Label(LabelRole::Body);
        QFont titleFont = m_title->font();
        titleFont.setPixelSize(CARD_TITLE_PX);
        titleFont.setWeight(font::DEMIBOLD);
        m_title->setFont(titleFont);

        m_usd = new Label(LabelRole::Mono);
        QFont usdFont = m_usd->font();
        usdFont.setPixelSize(CARD_USD_PX);
        m_usd->setFont(usdFont);

        m_addr = new Label(LabelRole::Mono);
        QFont addrFont = m_addr->font();
        addrFont.setPixelSize(CARD_ADDR_PX);
        m_addr->setFont(addrFont);
        m_addr->setWordWrap(true);

        (new qontrol::Column)
            ->margins(CARD_PAD_H, CARD_PAD_V, CARD_PAD_H, CARD_PAD_V)
            ->spacing(CARD_SPACING)
            ->push(m_title)
            ->push(m_usd)
            ->push(m_addr)
            ->into(this);

        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(CARD_BLUR);
        shadow->setOffset(0, CARD_SHADOW_DY);
        shadow->setColor(QColor(0, 0, 0, CARD_SHADOW_ALPHA));
        setGraphicsEffect(shadow);
    }

    void setData(const QString &title, const QString &usd, const QString &addr) {
        m_title->setText(title);
        m_usd->setText(usd);
        m_usd->setVisible(!usd.isEmpty());
        m_addr->setText(txMidAddr(addr));
        m_addr->setVisible(!addr.isEmpty());
        adjustSize();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto &p = Theme::get()->palette();
        QRectF box = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setBrush(p.surface);
        painter.setPen(QPen(p.border, 1));
        painter.drawRoundedRect(box, CARD_RADIUS, CARD_RADIUS);
    }

private:
    Label *m_title = nullptr;
    Label *m_usd = nullptr;
    Label *m_addr = nullptr;
};

TxFlow::TxFlow(QWidget *parent) : QWidget(parent) {
    setProperty("class", "tx-flow");
    setMouseTracking(true);
    // Fixed-height diagram (like the design's fixed viewBox height); pin it so a
    // constrained column (single-column layout) cannot collapse it to nothing.
    setFixedHeight(CANVAS_H);
    m_card = new TxFlowCard(this);
    m_card->hide();
}

TxFlow::~TxFlow() {
    // The card is reparented to the top-level window on hover, so it is not freed
    // with this widget's children; delete it explicitly.
    delete m_card;
}

void TxFlow::setInputs(const QList<InputItem> &inputs) {
    m_inputs.clear();
    for (const auto &item : inputs) {
        if (item.weight > 0.0) {
            m_inputs.append(item);
        }
    }
    m_hover_index = -1;
    m_card->hide();
    update();
}

void TxFlow::setOutputs(const QList<OutputItem> &outputs) {
    m_outputs.clear();
    for (const auto &item : outputs) {
        if (item.weight > 0.0) {
            m_outputs.append(item);
        }
    }
    m_hover_index = -1;
    m_card->hide();
    update();
}

void TxFlow::recomputeGeometry() {
    m_input_ribbons.clear();
    m_output_ribbons.clear();
    m_input_caps.clear();
    m_output_caps.clear();
    if (m_inputs.isEmpty() || m_outputs.isEmpty()) {
        return;
    }

    double trunkH = (CANVAS_H - 2.0 * PAD_Y) * TRUNK_FRAC;
    double trunkTop = (CANVAS_H - trunkH) / 2.0;
    double trunkBot = trunkTop + trunkH;

    QList<double> inW;
    for (const auto &item : m_inputs) {
        inW.append(item.weight);
    }
    QList<double> outW;
    for (const auto &item : m_outputs) {
        outW.append(item.weight);
    }

    auto inNode = txBands(inW, PAD_Y, CANVAS_H - PAD_Y, NODE_GAP);
    auto inTrunk = txBands(inW, trunkTop, trunkBot, 0.0);
    auto outNode = txBands(outW, PAD_Y, CANVAS_H - PAD_Y, NODE_GAP);
    auto outTrunk = txBands(outW, trunkTop, trunkBot, 0.0);

    for (int i = 0; i < m_inputs.size(); ++i) {
        m_input_ribbons.append(ribbonPath(X_FLOW_L, inNode[i].top, inNode[i].bot, X_TRUNK_L,
                                          inTrunk[i].top, inTrunk[i].bot));
        m_input_caps.append(QRectF(X_IN_STUB, inNode[i].top,
                                   X_FLOW_L - X_IN_STUB - CAP_INSET,
                                   inNode[i].bot - inNode[i].top));
    }
    for (int j = 0; j < m_outputs.size(); ++j) {
        m_output_ribbons.append(ribbonPath(X_TRUNK_R, outTrunk[j].top, outTrunk[j].bot,
                                           X_FLOW_R, outNode[j].top, outNode[j].bot));
        m_output_caps.append(QRectF(X_FLOW_R + CAP_INSET, outNode[j].top,
                                    X_OUT_STUB - X_FLOW_R - CAP_INSET,
                                    outNode[j].bot - outNode[j].top));
    }
}

void TxFlow::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto &p = Theme::get()->palette();

    if (m_inputs.isEmpty() || m_outputs.isEmpty()) {
        m_card->hide();
        // Centered placeholder: muted stubs, trunk, dashed connectors, body text.
        double cx = width() / 2.0;
        double cy = CANVAS_H / 2.0;
        QColor muted = p.textMuted;
        muted.setAlphaF(EMPTY_ALPHA);
        painter.setPen(Qt::NoPen);
        painter.setBrush(muted);
        painter.drawRoundedRect(
            QRectF(cx + EMPTY_STUB_X, cy + EMPTY_STUB_TOP_Y, EMPTY_STUB_W, EMPTY_STUB_H),
            CAP_RADIUS, CAP_RADIUS);
        painter.drawRoundedRect(
            QRectF(cx + EMPTY_STUB_X, cy + EMPTY_STUB_BOT_Y, EMPTY_STUB_W, EMPTY_STUB_H),
            CAP_RADIUS, CAP_RADIUS);
        QColor trunk = p.textMuted;
        trunk.setAlphaF(EMPTY_TRUNK_ALPHA);
        painter.setBrush(trunk);
        painter.drawRoundedRect(
            QRectF(cx + EMPTY_TRUNK_X, cy + EMPTY_TRUNK_Y, EMPTY_TRUNK_W, EMPTY_TRUNK_H),
            CAP_RADIUS, CAP_RADIUS);
        painter.setBrush(muted);
        painter.drawRoundedRect(
            QRectF(cx + EMPTY_OUT_X, cy + EMPTY_OUT_Y, EMPTY_STUB_W, EMPTY_OUT_H),
            CAP_RADIUS, CAP_RADIUS);
        QColor dash = p.textMuted;
        dash.setAlphaF(EMPTY_DASH_ALPHA);
        QPen dashPen(dash, EMPTY_DASH_WIDTH);
        dashPen.setStyle(Qt::CustomDashLine);
        dashPen.setDashPattern({EMPTY_DASH_ON, EMPTY_DASH_OFF});
        painter.setPen(dashPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(cx + EMPTY_CONN_IN_X, cy + EMPTY_CONN_TOP_Y0),
                         QPointF(cx + EMPTY_TRUNK_X, cy + EMPTY_CONN_TOP_Y1));
        painter.drawLine(QPointF(cx + EMPTY_CONN_IN_X, cy + EMPTY_CONN_BOT_Y0),
                         QPointF(cx + EMPTY_TRUNK_X, cy + EMPTY_CONN_BOT_Y1));
        painter.drawLine(QPointF(cx + EMPTY_CONN_OUT_X0, cy),
                         QPointF(cx + EMPTY_CONN_OUT_X1, cy));
        painter.setPen(p.textMuted);
        painter.drawText(QRectF(0, cy + EMPTY_TEXT_TOP, width(), CANVAS_H / 2.0 - EMPTY_TEXT_TOP),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, TR("txflow-empty"));
        return;
    }

    recomputeGeometry();

    GraphX gx = graphX(width());
    double trunkH = (CANVAS_H - 2.0 * PAD_Y) * TRUNK_FRAC;
    double trunkTop = (CANVAS_H - trunkH) / 2.0;

    // Geometry is drawn in the centered, scaled virtual space; text is drawn
    // afterwards in device space so glyphs are not horizontally stretched.
    painter.save();
    painter.translate(gx.offset, 0.0);
    painter.scale(gx.scale, 1.0);
    painter.setPen(Qt::NoPen);

    // (1) input ribbons
    for (int i = 0; i < m_input_ribbons.size(); ++i) {
        QColor fill = p.txSelf;
        bool hot = m_hover_is_input && m_hover_index == i;
        fill.setAlphaF(hot ? RIBBON_HOVER_ALPHA
                           : RIBBON_IN_BASE + RIBBON_IN_STEP * (i % RIBBON_ALPHA_CYCLE));
        painter.setBrush(fill);
        painter.drawPath(m_input_ribbons[i]);
    }

    // (2) trunk
    QColor trunkFill = p.txSelf;
    trunkFill.setAlphaF(TRUNK_ALPHA);
    painter.setBrush(trunkFill);
    painter.drawRect(QRectF(X_TRUNK_L, trunkTop, X_TRUNK_R - X_TRUNK_L, trunkH));

    // (3) output ribbons
    for (int j = 0; j < m_output_ribbons.size(); ++j) {
        QColor fill = kindFill(p, m_outputs[j].kind);
        bool hot = !m_hover_is_input && m_hover_index == j;
        fill.setAlphaF(hot ? RIBBON_HOVER_ALPHA : RIBBON_OUT_ALPHA);
        painter.setBrush(fill);
        painter.drawPath(m_output_ribbons[j]);
    }

    // (4) caps
    painter.setBrush(p.txSelf);
    for (const auto &cap : m_input_caps) {
        painter.drawRoundedRect(cap, CAP_RADIUS, CAP_RADIUS);
    }
    for (int j = 0; j < m_output_caps.size(); ++j) {
        painter.setBrush(kindFill(p, m_outputs[j].kind));
        painter.drawRoundedRect(m_output_caps[j], CAP_RADIUS, CAP_RADIUS);
    }

    painter.restore();

    // (5) on-ribbon output labels in device space, right-anchored at scaled X.
    QFont labelFont(font::MONO, LABEL_FONT_PX);
    labelFont.setWeight(font::DEMIBOLD);
    labelFont.setLetterSpacing(QFont::PercentageSpacing, LABEL_SPACING);
    painter.setFont(labelFont);
    double anchorX = gx.offset + (X_FLOW_R - LABEL_INSET) * gx.scale;
    for (int j = 0; j < m_outputs.size(); ++j) {
        double mid = m_output_caps[j].center().y();
        painter.setPen(kindLabelColor(p, m_outputs[j].kind));
        QRectF box(0, mid - LABEL_FONT_PX, anchorX, 2.0 * LABEL_FONT_PX);
        painter.drawText(box, Qt::AlignRight | Qt::AlignVCenter, kindLabelText(m_outputs[j]));
    }
}

auto TxFlow::sizeHint() const -> QSize {
    return {static_cast<int>(CANVAS_W), CANVAS_H};
}

auto TxFlow::hitTest(const QPointF &pos) const -> HoverHit {
    GraphX gx = graphX(width());
    double vx = (pos.x() - gx.offset) / gx.scale;
    double vy = pos.y();
    QPointF vpos(vx, vy);

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

void TxFlow::positionCard(QMouseEvent *event) {
    // Float the card over the whole window so it is not clipped by the diagram and
    // can extend past the graph card when tall. Reparent to the top level once.
    QWidget *top = window();
    if (m_card->parentWidget() != top) {
        m_card->setParent(top);
    }
    QPoint cursor = mapTo(top, event->position().toPoint());
    int cardL = cursor.x() + CARD_OFFSET;
    if (cardL + CARD_WIDTH > top->width()) {
        cardL = cursor.x() - CARD_OFFSET - CARD_WIDTH;
    }
    cardL = std::clamp(cardL, CARD_MARGIN,
                       std::max(CARD_MARGIN, top->width() - CARD_WIDTH - CARD_MARGIN));
    int cardT = std::clamp(cursor.y() - CARD_OFFSET, CARD_MARGIN,
                           std::max(CARD_MARGIN, top->height() - m_card->height() - CARD_MARGIN));
    m_card->move(cardL, cardT);
    m_card->raise();
    m_card->show();
}

void TxFlow::mouseMoveEvent(QMouseEvent *event) {
    if (m_inputs.isEmpty() || m_outputs.isEmpty() || m_input_ribbons.isEmpty()) {
        return;
    }

    HoverHit hit = hitTest(event->position());

    if (hit.index < 0) {
        if (m_hover_index >= 0) {
            m_hover_index = -1;
            m_card->hide();
            update();
        }
        return;
    }

    m_hover_index = hit.index;
    m_hover_is_input = hit.is_input;
    if (hit.is_input) {
        const auto &item = m_inputs[hit.index];
        m_card->setData(item.title, item.usd, item.addr);
    } else {
        const auto &item = m_outputs[hit.index];
        m_card->setData(item.title, item.usd, item.addr);
    }
    update();

    positionCard(event);
}

void TxFlow::leaveEvent(QEvent *event) {
    if (m_hover_index >= 0) {
        m_hover_index = -1;
        m_card->hide();
        update();
    }
    QWidget::leaveEvent(event);
}

} // namespace catalog

#include "TxFlow.moc"
