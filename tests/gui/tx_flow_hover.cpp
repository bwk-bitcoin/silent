#include "tx_flow_hover.h"

#include "catalog/containers/Modal.h"
#include "catalog/display/Amount.h"
#include "catalog/display/Label.h"
#include "catalog/panels/send/TxFlow.h"

#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QWidget>
#include <algorithm>
#include <cstdio>

namespace {

constexpr int SETTLE_TICKS = 12;
constexpr int MOVES = 20;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("TxFlow hover: %s", message);
    }
}

void settle() {
    for (int tick = 0; tick < SETTLE_TICKS; ++tick) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}

class Flow : public catalog::TxFlow {
public:
    auto pointFor(int index, bool is_input) -> QPoint {
        recomputeGeometry(canvasMap().width);
        for (int y = 1; y < height() - 1; ++y) {
            for (int x = 1; x < width() - 1; ++x) {
                auto hit = hitTest(QPoint(x, y));
                auto next = hitTest(QPoint(x + 1, y));
                if (hit.index == index && hit.is_input == is_input && next.index == index &&
                    next.is_input == is_input) {
                    return {x, y};
                }
            }
        }
        qFatal("TxFlow hover: no hit for %s %d", is_input ? "input" : "output", index);
    }
};

class Events : public QObject {
public:
    int layouts = 0;
    int resizes = 0;
    int moves = 0;

    auto eventFilter([[maybe_unused]] QObject *object, QEvent *event) -> bool override {
        switch (event->type()) {
        case QEvent::LayoutRequest:
            ++layouts;
            break;
        case QEvent::Resize:
            ++resizes;
            break;
        case QEvent::Move:
            ++moves;
            break;
        default:
            break;
        }
        return false;
    }

    void reset() {
        layouts = 0;
        resizes = 0;
        moves = 0;
    }

    void requireStable() const {
        require(layouts == 0, "hover must not request a modal refit");
        require(resizes == 0, "hover must not resize the modal");
        require(moves == 0, "hover must not move the modal");
    }
};

void moveTo(Flow *flow, QPoint point) {
    QMouseEvent event(QEvent::MouseMove, point, flow->mapToGlobal(point), Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(flow, &event);
    settle();
}

auto hoverCard(QWidget *window) -> QWidget * {
    for (auto *widget : window->findChildren<QWidget *>()) {
        if (QByteArray(widget->metaObject()->className()) == "catalog::TxFlowHoverCard") {
            return widget;
        }
    }
    qFatal("TxFlow hover: missing hover card");
}

auto hasText(QWidget *card, const QString &text) -> bool {
    return std::ranges::any_of(card->findChildren<catalog::Label *>(), [&text](auto *label) {
        return label->isVisible() && label->text() == text;
    });
}

void checkFlow(bool detail) {
    catalog::Modal modal;
    modal.setFixedWidth(820);
    modal.setWizardHeight(true);
    auto *flow = new Flow;
    flow->setDetail(detail);
    QList<catalog::TxFlow::InputItem> inputs{
        {.weight = 50, .title = "Input zero", .fiat = "12.00 EUR", .addr = QString(64, 'a')},
        {.weight = 50, .title = "Input one", .value_known = false}};
    QList<catalog::TxFlow::OutputItem> outputs{
        {.weight = 75, .title = "Output zero", .fiat = "18.00 EUR", .addr = QString(96, 'b')},
        {.kind = catalog::TxFlow::OutputKind::Fee, .weight = 25, .title = "Fee"}};
    flow->setInputs(inputs);
    flow->setOutputs(outputs);
    modal.setBody(flow);
    modal.show();
    settle();

    Events modalEvents;
    modal.installEventFilter(&modalEvents);
    QPointer<QWidget> card = hoverCard(&modal);
    auto input0 = flow->pointFor(0, true);
    auto input1 = flow->pointFor(1, true);
    auto output0 = flow->pointFor(0, false);
    auto output1 = flow->pointFor(1, false);
    moveTo(flow, input0);
    require(card->isVisible() && hasText(card, "Input zero"), "first hover shows input zero");
    require(card->testAttribute(Qt::WA_TransparentForMouseEvents), "card stays mouse-transparent");
    modalEvents.requireStable();

    Events cardEvents;
    card->installEventFilter(&cardEvents);
    auto originalPosition = card->pos();
    for (int i = 0; i < MOVES; ++i) {
        moveTo(flow, input0 + QPoint(i % 2, 0));
    }
    require(cardEvents.layouts == 0, "same input must not relayout the card");
    require(card->pos() != originalPosition, "card still follows moves within one input");
    modalEvents.requireStable();

    moveTo(flow, output0);
    require(hasText(card, "Output zero"), "same index on the output side refreshes content");
    require(hasText(card, "18.00 EUR"), "known output value is visible");
    modalEvents.requireStable();
    cardEvents.reset();
    for (int i = 0; i < MOVES; ++i) {
        moveTo(flow, output0 + QPoint(i % 2, 0));
    }
    require(cardEvents.layouts == 0, "same output must not relayout the card");
    modalEvents.requireStable();

    moveTo(flow, output1);
    require(hasText(card, "Fee"), "switching output index refreshes content");
    moveTo(flow, input1);
    require(hasText(card, "Input one") && hasText(card, "Unknown"),
            "unknown input keeps its value feedback");
    modalEvents.requireStable();

    moveTo(flow, input0);
    catalog::AmountDisplay::get()->cycle();
    settle();
    require(hasText(card, "12.00 EUR") != detail, "hidden mode refreshes only detail values");
    catalog::AmountDisplay::get()->cycle();
    settle();
    require(hasText(card, "12.00 EUR"), "restoring BTC refreshes a stationary hover");
    modalEvents.requireStable();

    inputs[0].title = "Updated input";
    inputs[0].fiat = "24.00 EUR";
    flow->setInputs(inputs);
    settle();
    require(!card->isVisible(), "live input replacement clears the previous hover");
    moveTo(flow, input0);
    require(hasText(card, "Updated input") && hasText(card, "24.00 EUR"),
            "same index uses replaced input data");
    outputs[0].title = "Updated output";
    flow->setOutputs(outputs);
    moveTo(flow, output0);
    require(hasText(card, "Updated output"), "same index uses replaced output data");
    modalEvents.requireStable();

    moveTo(flow, QPoint(flow->width() / 2, 0));
    require(!card->isVisible(), "moving off the bands hides the card");
    moveTo(flow, input0);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(flow, &leave);
    settle();
    require(!card->isVisible(), "leaving the diagram hides the card");
    moveTo(flow, input0);
    modalEvents.requireStable();
    flow->hide();
    settle();
    require(!card->isVisible(), "hiding the diagram hides its window-level card");
    flow->show();
    settle();
    modalEvents.reset();
    moveTo(flow, input0);
    require(card->isVisible(), "hover works after the diagram is shown again");
    modalEvents.requireStable();
    flow->setInputs({});
    settle();
    require(!card->isVisible(), "empty data clears the hover card");
    flow->setInputs(inputs);
    moveTo(flow, flow->pointFor(0, true));
    require(card->isVisible(), "destruction starts with a visible hover card");
    delete flow;
    require(card.isNull(), "destroying the flow releases its window-level card");
    std::printf("PASS TxFlow %s: first show, %d input moves, %d output moves, switches, live data, "
                "amount mode, leave, hide/show, destruction\n",
                detail ? "detail" : "send", MOVES, MOVES);
}

void checkScrollAndResize(bool detail) {
    QWidget window;
    window.resize(820, 500);
    QScrollArea scroll(&window);
    scroll.setGeometry(10, 10, 800, 480);
    auto *body = new QWidget;
    body->setMinimumSize(760, 1200);
    auto *flow = new Flow;
    flow->setParent(body);
    flow->setDetail(detail);
    flow->setGeometry(10, 100, 720, flow->height());
    flow->setInputs({{.weight = 50, .title = "Scrolled input", .addr = QString(96, 'a')}});
    flow->setOutputs({{.weight = 50, .title = "Scrolled output"}});
    scroll.setWidget(body);
    window.show();
    settle();
    QPointer<QWidget> card = hoverCard(&window);
    QPointer<QWidget> layer = card->parentWidget();
    auto point = flow->pointFor(0, true);
    moveTo(flow, point);
    require(card->isVisible(), "scroll fixture starts hovered");
    auto *button = new QPushButton("Sibling", body);
    button->setGeometry(
        QRect(body->mapFrom(&window, card->mapTo(&window, QPoint(20, 20))), QSize(80, 25)));
    button->show();
    settle();
    auto buttonCenter = button->mapToGlobal(button->rect().center());
    require(card->rect().contains(card->mapFromGlobal(buttonCenter)),
            "sibling button lies beneath the visible card");
    require(QApplication::widgetAt(buttonCenter) == button,
            "window hit testing passes through the card and layer to a sibling button");
    delete button;

    scroll.verticalScrollBar()->setValue(40);
    settle();
    auto position = scroll.verticalScrollBar()->value();
    require(position == 40, "scroll fixture reaches its requested position");
    for (int i = 0; i < MOVES; ++i) {
        moveTo(flow, point + QPoint(i % 2, 0));
    }
    require(scroll.verticalScrollBar()->value() == position,
            "hover movement preserves the requested scrollbar position");
    auto expectedY = std::clamp(flow->mapTo(&window, QPoint(0, std::max(4, point.y() - 14))).y(), 4,
                                std::max(4, window.height() - card->height() - 4));
    require(card->y() == expectedY, "hover movement relocates the card after scrolling");

    window.resize(580, 360);
    scroll.setGeometry(10, 10, 560, 340);
    flow->resize(520, flow->height());
    settle();
    moveTo(flow, flow->pointFor(0, false));
    require(layer->geometry() == window.rect(), "hover after resize uses the current window size");
    require(window.rect().adjusted(4, 4, -4, -4).contains(card->geometry()),
            "hover after resize clamps the card to the current window margins");
    require(card->isVisible(), "close starts with a visible hover card");
    window.close();
    settle();
    require(!card->isVisible(), "closing the window hides its hover card");
    delete flow;
    require(card.isNull() && layer.isNull(), "hovered flow teardown releases card and layer");
    std::printf("PASS TxFlow %s: sibling hit testing, scroll position, relocation, resize clamp, "
                "hovered close and layer destruction\n",
                detail ? "detail" : "send");
}

void checkStationaryResize(bool detail) {
    auto *window = new QWidget;
    window->resize(820, 500);
    auto *flow = new Flow;
    flow->setParent(window);
    flow->setDetail(detail);
    flow->setGeometry(20, 120, 720, flow->height());
    flow->setInputs({{.weight = 50, .title = "Stationary input"}});
    flow->setOutputs({{.weight = 50, .title = "Stationary output", .addr = QString(96, 'b')}});
    window->show();
    settle();
    moveTo(flow, flow->pointFor(0, false));
    QPointer<QWidget> card = hoverCard(window);
    QPointer<QWidget> layer = card->parentWidget();
    require(card->isVisible() && card->geometry().right() > 580,
            "stationary width fixture starts with a card beyond the future window edge");
    auto flowSize = flow->size();
    Events cardEvents;
    card->installEventFilter(&cardEvents);
    window->resize(580, 500);
    settle();
    require(card->isVisible() && layer->geometry() == window->rect() &&
                window->rect().adjusted(4, 4, -4, -4).contains(card->geometry()),
            "stationary width shrink clamps the visible card and resizes its layer");
    require(card->geometry().bottom() > 200,
            "stationary height fixture starts with a card beyond the future window edge");
    window->resize(580, 200);
    settle();
    require(card->isVisible() && layer->geometry() == window->rect() &&
                window->rect().adjusted(4, 4, -4, -4).contains(card->geometry()),
            "stationary height shrink clamps the visible card and resizes its layer");
    require(flow->size() == flowSize, "window resize clamping does not depend on a flow resize");
    require(cardEvents.layouts == 0 && hasText(card, "Stationary output"),
            "stationary clamping preserves hover identity without relaying out the card");

    auto *nextWindow = new QWidget;
    nextWindow->resize(820, 500);
    flow->setParent(nextWindow);
    window->resize(440, 180);
    settle();
    delete window;
    require(!card.isNull() && !layer.isNull() && !card->isVisible(),
            "destroying the old window preserves a hidden reparented flow's card and layer");
    nextWindow->show();
    flow->show();
    settle();
    require(layer->parentWidget() == nextWindow,
            "reparented flow moves its layer to the new window");
    moveTo(flow, flow->pointFor(0, false));
    require(layer->geometry() == nextWindow->rect() && card->isVisible(),
            "hover resumes in the new window after the old window is destroyed");
    nextWindow->resize(580, 200);
    settle();
    require(card->isVisible() && layer->geometry() == nextWindow->rect() &&
                nextWindow->rect().adjusted(4, 4, -4, -4).contains(card->geometry()),
            "new window resizing clamps the stationary reparented hover");
    delete nextWindow;
    require(card.isNull() && layer.isNull(), "destroying a hovered window releases card and layer");
    std::printf("PASS TxFlow %s: stationary width/height shrink, unchanged flow size, reparented "
                "window filter, hovered window destruction\n",
                detail ? "detail" : "send");
}

} // namespace

namespace smoke {

void txFlowHoverSmoke() {
    QWidget window;
    window.resize(1024, 768);
    window.show();
    settle();
    checkFlow(true);
    checkFlow(false);
    checkScrollAndResize(true);
    checkScrollAndResize(false);
    checkStationaryResize(true);
    checkStationaryResize(false);
}

} // namespace smoke
