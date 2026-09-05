#include "Send.h"
#include "AccountController.h"
#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Table.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Display.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/form/LabelledInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/Toggle.h"
#include "catalog/panels/send/TxFlow.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "theme/Theme.h"
#include "views/modals/ConfirmSend.h"
#include "views/modals/SelectSigner.h"
#include "views/utils.h"
#include <QApplication>
#include <QBoxLayout>
#include <QDebug>
#include <QDoubleValidator>
#include <QEvent>
#include <QIntValidator>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QThread>
#include <Qontrol>
#include <QtGlobal>
#include <algorithm>
#include <common.h>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <qcheckbox.h>
#include <qcontainerfwd.h>
#include <string>

namespace view {

namespace {

const double USD_RATE = 67000.0;

// Selected fiat currency 3-letter code. Currency selection and the live
// conversion rate are not yet wired, so this is a fixed default for now.
const char *const FIAT_CODE = "USD";

const int LEGEND_SWATCH = 8;        // legend color swatch size (design 8x8)
const int LEGEND_SWATCH_RADIUS = 2; // legend swatch corner radius

// Tiny colored swatch painted in a TxFlow output kind color; resolves the
// palette at paint time so it tracks theme changes.
class LegendSwatch : public QWidget {
public:
    explicit LegendSwatch(catalog::TxFlow::OutputKind kind, QWidget *parent = nullptr)
        : QWidget(parent),
          m_kind(kind) {
        setFixedSize(LEGEND_SWATCH, LEGEND_SWATCH);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto &p = Theme::get()->palette();
        QColor fill = p.txWarning;
        if (m_kind == catalog::TxFlow::OutputKind::Change) {
            fill = p.txSelf;
        } else if (m_kind == catalog::TxFlow::OutputKind::Fee) {
            fill = p.error;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(rect(), LEGEND_SWATCH_RADIUS, LEGEND_SWATCH_RADIUS);
    }

private:
    catalog::TxFlow::OutputKind m_kind;
};

auto usdAmount(uint64_t sats) -> QString {
    double btc = static_cast<double>(sats) / SATS;
    return QString("~ $%1").arg(QString::number(btc * USD_RATE, 'f', 2));
}

auto btcFromUsd(const QString &usd_text) -> QString {
    bool ok = false;
    double usd = usd_text.toDouble(&ok);
    if (!ok || usd <= 0.0) {
        return QString();
    }
    return QString::number(usd / USD_RATE, 'f', 8);
}

} // namespace

using catalog::Button;
using catalog::ButtonRole;
using catalog::Checkbox;
using catalog::DisplayRole;
using catalog::Input;
using catalog::InputRole;
using catalog::Label;
using catalog::LabelRole;
using catalog::Toggle;

static void setValidationIndicator(catalog::ValidationMark *indicator, const QString &text,
                                   bool valid) {
    if (text.isEmpty()) {
        indicator->setState(catalog::ValidationMark::State::None);
        return;
    }
    if (valid) {
        indicator->setState(catalog::ValidationMark::State::Valid);
    } else {
        indicator->setState(catalog::ValidationMark::State::Invalid);
    }
}

InputW::InputW(const RustCoin &coin) {
    auto *typeLabel =
        new Label(QString::fromUtf8(coin.account_type.data(), coin.account_type.size()));

    auto *outpointLabel =
        new Label(QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size()), LabelRole::Mono);

    auto *valueLabel = new Label(toBitcoin(coin.value), LabelRole::Mono);
    valueLabel->setAlignment(Qt::AlignRight);

    auto *labelLabel = new Label(QString::fromUtf8(coin.label.data(), coin.label.size()));

    auto *row = (new qontrol::Row)
                    ->push(typeLabel)
                    ->pushSpacer(resolve(Spacing::XS))
                    ->push(outpointLabel)
                    ->pushSpacer(resolve(Spacing::XS))
                    ->push(valueLabel)
                    ->pushSpacer(resolve(Spacing::XS))
                    ->push(labelLabel)
                    ->pushSpacer();

    m_widget = row;
}

auto InputW::widget() -> QWidget * {
    return m_widget;
}

OutputW::OutputW(Send *screen, int id) {
    m_address_input = new Input(InputRole::Mono);
    m_address_input->setPlaceholderText(TR("send-placeholder-address"));

    m_address_indicator = new catalog::ValidationMark;

    m_delete_btn = new Button(ButtonRole::InlineIcon);
    m_delete_btn->setProperty("overlay", true); // transparent: no bg/border
    m_delete_btn->setIcon(icon::close(IconColor::Muted));

    m_amount_input = new Input(InputRole::Mono);
    m_amount_input->setWidth(Size::S);
    m_amount_input->setPlaceholderText(TR("send-placeholder-amount"));
    m_amount_usd_input = new Input(InputRole::Mono);
    m_amount_usd_input->setWidth(Size::S);
    m_amount_usd_input->setPlaceholderText(TR("send-summary-usd-placeholder"));

    m_amount_indicator = new catalog::ValidationMark;

    m_label_input = new Input;
    m_label_input->setPlaceholderText(TR("send-placeholder-label"));

    m_max = new Checkbox;
    QObject::connect(m_max, &Checkbox::toggled, screen, &Send::onProcess, qontrol::UNIQUE);

    m_max_label = new Label(TR("send-max"), LabelRole::Body);

    // Connect to onProcess() which handles validation updates
    QObject::connect(m_address_input, &QLineEdit::textChanged, screen, &Send::onProcess,
                     qontrol::UNIQUE);
    QObject::connect(m_amount_input, &QLineEdit::textChanged, screen, &Send::onProcess,
                     qontrol::UNIQUE);
    QObject::connect(m_amount_usd_input, &QLineEdit::textChanged, screen, &Send::onProcess,
                     qontrol::UNIQUE);
    // The label feeds the recipient's on-ribbon text in the flow diagram.
    QObject::connect(m_label_input, &QLineEdit::textChanged, screen, &Send::onProcess,
                     qontrol::UNIQUE);

    // Amount field: a label on top of a row of tight unit-paired groups. Each
    // value is glued to its unit (input+BTC, fiat+USD, checkbox+MAX) with a tiny
    // gap; the groups are separated by a normal gap. Units and MAX align to the
    // bottom of the row, level with the inputs.
    auto *btcCode = new Label("BTC", LabelRole::Subheading);
    auto *fiatCode = new Label(FIAT_CODE, LabelRole::Subheading);

    // Match LabelledInput's top label (small caption recolored to secondary).
    auto *amountLabel = new Label(TR("send-amount"), LabelRole::Caption);
    amountLabel->setProperty("labelKind", "labelled");

    auto *btcGroup = (new qontrol::Row)
                         ->spacing(resolve(Spacing::XS))
                         ->push(m_amount_input)
                         ->push(btcCode, 0, Qt::AlignBottom)
                         ->push(m_amount_indicator, 0, Qt::AlignBottom);

    auto *usdGroup = (new qontrol::Row)
                         ->spacing(resolve(Spacing::XS))
                         ->push(m_amount_usd_input)
                         ->push(fiatCode, 0, Qt::AlignBottom);

    auto *maxGroup = (new qontrol::Row)
                         ->spacing(resolve(Spacing::XS))
                         ->push(m_max, 0, Qt::AlignBottom)
                         ->push(m_max_label, 0, Qt::AlignBottom);

    auto *amountRow = (new qontrol::Row)
                          ->push(btcGroup)
                          ->pushSpacer(resolve(Spacing::S))
                          ->push(usdGroup)
                          ->pushSpacer(resolve(Spacing::S))
                          ->push(maxGroup)
                          ->pushSpacer();

    auto *amountField = (new qontrol::Column)
                            ->spacing(resolve(Spacing::XXS))
                            ->push(amountLabel)
                            ->push(amountRow);

    auto *content = (new qontrol::Column)
                        ->push(new catalog::LabelledInput(TR("send-address"), m_address_input,
                                                          m_address_indicator))
                        ->pushSpacer(resolve(Spacing::XS))
                        ->push(new catalog::LabelledInput(TR("send-label"), m_label_input))
                        ->pushSpacer(resolve(Spacing::XS))
                        ->push(amountField);

    auto *card = new catalog::Card;
    card->setContent(content);
    m_widget = card;

    // Recipient card keeps 20px horizontal padding (design --pad-l), with a
    // tighter top/bottom padding than the shared Card default.
    card->setContentMargins(resolve(Padding::L), resolve(Padding::S), resolve(Padding::L),
                            resolve(Padding::S));

    // Float the delete button over the card's top-right corner: a child of the
    // card (over its padding), repositioned in eventFilter on resize, so it
    // ignores the content padding and never affects the layout flow.
    m_delete_btn->setParent(card);
    m_delete_btn->raise();
    card->installEventFilter(this);
    m_delete_btn->installEventFilter(this);

    m_delete_btn->setProperty("outputId", id);
    QObject::connect(m_delete_btn, &QPushButton::clicked, screen, &Send::onOutputDeleteClicked,
                     qontrol::UNIQUE);

    m_max->setProperty("outputId", id);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QObject::connect(m_max, &QCheckBox::checkStateChanged, screen, &Send::onOutputMaxToggled,
                     qontrol::UNIQUE);
#else
    QObject::connect(m_max, &QCheckBox::stateChanged, screen, &Send::onOutputMaxToggled,
                     qontrol::UNIQUE);
#endif
}

void OutputW::positionDeleteButton() {
    if (m_widget == nullptr) {
        return;
    }
    int margin = resolve(Spacing::XS);
    m_delete_btn->move(m_widget->width() - m_delete_btn->width() - margin, margin);
    m_delete_btn->raise();
}

auto OutputW::eventFilter(QObject *watched, QEvent *event) -> bool {
    if (watched == m_widget && event->type() == QEvent::Resize) {
        positionDeleteButton();
    } else if (watched == m_delete_btn) {
        if (event->type() == QEvent::Enter) {
            m_delete_btn->setIcon(icon::close(IconColor::Default));
        } else if (event->type() == QEvent::Leave) {
            m_delete_btn->setIcon(icon::close(IconColor::Muted));
        } else if (event->type() == QEvent::MouseButtonPress) {
            m_delete_btn->setIcon(icon::close(IconColor::Error));
        }
    }
    return QObject::eventFilter(watched, event);
}

auto OutputW::widget() -> QWidget * {
    return m_widget;
}

void OutputW::setDeletable(bool deletable) {
    m_delete_btn->setVisible(deletable);
}

void OutputW::enableMax(bool max) {
    m_max->setChecked(false);
    m_max->setVisible(max);
    m_max_label->setVisible(max);
}

void OutputW::setMaxMode(bool max) {
    m_amount_input->setEnabled(!max);
    if (max) {
        m_amount_input->setText(TR("send-max"));
        setValidationIndicator(m_amount_indicator, "", false);
    } else {
        if (m_amount_input->text() == TR("send-max")) {
            m_amount_input->setText("");
        }
        m_amount_input->setPlaceholderText(TR("send-placeholder-amount"));
    }
}

void OutputW::setMaxAmount(uint64_t sats) {
    m_amount_input->setText(toBitcoin(sats, false));
}

auto OutputW::isMax() -> bool {
    return m_max->isChecked();
}

void Send::onFeeToggled() {
    qDebug() << "Send::onFeeToggled()";
    if (m_fee_toggle->isChecked()) {
        m_fee_label->setText(TR("send-fee-rate-unit"));
        // sats/vb: 3 decimal places max (milli-sats precision)
        auto *validator = new QDoubleValidator(0.001, 1000000.0, 3, m_fee_value_input);
        validator->setNotation(QDoubleValidator::StandardNotation);
        m_fee_value_input->setValidator(validator);
    } else {
        m_fee_label->setText(TR("send-fee-absolute-unit"));
        // sats: integers only
        m_fee_value_input->setValidator(new QIntValidator(1, 100000000, m_fee_value_input));
    }
    onProcess();
}

Send::Send(AccountController *ctrl) {
    qDebug() << "Send::Send()";
    m_controller = ctrl;
    if (m_controller == nullptr) {
        return;
    }
    this->init();
    this->doConnect();
    this->onAddOutput();
    this->view();
    this->onFeeToggled();
    this->onSetBroadcastable(false);
}

void Send::init() {
    qDebug() << "Send::init()";
    m_outputs_column = (new qontrol::Column);
    m_inputs_column = (new qontrol::Column);

    m_add_output_btn = new Button(TR("send-add-output"));
    m_send_btn = new Button(TR("send-action-sign"), ButtonRole::Primary);
    m_clear_outputs_btn = new Button(TR("common-clear"));

    m_fee_estimate_label = new Label();
    m_fee_estimate_label->setVisible(false);

    // Fee toggle: OFF = sats (absolute), ON = sats/vb (rate)
    m_fee_toggle = new Toggle;
    m_fee_toggle->setChecked(true); // Default to sats/vb mode

    m_fee_value_input = new Input(InputRole::Mono);
    m_fee_value_input->setWidth(Size::XS);
    m_fee_value_input->setText("1");

    m_fee_indicator = new catalog::ValidationMark;

    m_fee_label = new Label(TR("send-fee-rate-unit"));
    m_fee_label->setFixedWidth(
        m_fee_label->fontMetrics().horizontalAdvance(TR("send-fee-rate-unit")) + 5);

    m_warning_label = new Label();
    m_warning_label->setVisible(false);

    // Calculate label width to align with coin value column
    // checkbox + spacer + type + spacer + outpoint + spacer + label = total label width
    // "Remaining to select" and "Selected" as two equal-width halves split by a
    // vertical rule, so the rule sits at the row centre.
    auto *minLabel = new Label(TR("send-amount-to-select"), LabelRole::InfoLabel);
    m_inputs_min_value = new Label(QString(), LabelRole::Mono);
    auto *totalLabel = new Label(TR("send-total-selected"), LabelRole::InfoLabel);
    m_inputs_total_value = new Label(QString(), LabelRole::Mono);

    auto *leftHalf = (new qontrol::Row)
                         ->push(minLabel)
                         ->pushSpacer(resolve(Spacing::XS))
                         ->push(m_inputs_min_value)
                         ->pushSpacer();
    auto *rightHalf = (new qontrol::Row)
                          ->pushSpacer()
                          ->push(totalLabel)
                          ->pushSpacer(resolve(Spacing::XS))
                          ->push(m_inputs_total_value);
    auto *amountsRow = (new qontrol::Row)
                           ->push(leftHalf)
                           ->pushSpacer(resolve(Spacing::S))
                           ->push(new catalog::Separator(catalog::Separator::Role::Vertical))
                           ->pushSpacer(resolve(Spacing::S))
                           ->push(rightHalf);
    // Equal-width halves so the divider is centred regardless of content width.
    for (QWidget *half : {static_cast<QWidget *>(leftHalf), static_cast<QWidget *>(rightHalf)}) {
        auto policy = half->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        half->setSizePolicy(policy);
    }
    if (auto *box = qobject_cast<QBoxLayout *>(amountsRow->layout())) {
        // Match the table rows' horizontal padding so the right-aligned total
        // selected value lines up under the value column, and the left label
        // lines up with the checkbox column.
        box->setContentsMargins(resolve(Padding::S), 0, resolve(Padding::S), 0);
        box->setStretchFactor(leftHalf, 1);
        box->setStretchFactor(rightHalf, 1);
    }
    m_inputs_amounts_row = amountsRow;

    m_auto_coin_selection = new Checkbox(TR("send-auto-coin-selection"));
    m_auto_coin_selection->setChecked(true);

    m_clear_inputs_btn = new Button(TR("common-clear"), ButtonRole::Inline);

    m_inputs_title_label = new Label(TR("send-select-inputs"), LabelRole::Heading);
    // Pin the title to the button's height so the row does not change height when
    // the (compact) Clear button shows/hides. defaultBtn has no fixed height, so
    // the effective button height is BUTTON_MIN_HEIGHT.
    m_inputs_title_label->setMinimumHeight(metric::BUTTON_MIN_HEIGHT);
}

void Send::doConnect() {
    qDebug() << "Send::doConnect()";
    connect(m_add_output_btn, &QPushButton::clicked, this, &Send::onAddOutput, qontrol::UNIQUE);
    connect(m_send_btn, &QPushButton::clicked, this, &Send::onSendTransaction, qontrol::UNIQUE);
    connect(m_clear_outputs_btn, &QPushButton::clicked, this, &Send::onClearOutputs,
            qontrol::UNIQUE);
    connect(m_clear_inputs_btn, &QPushButton::clicked, this, &Send::onClearInputs, qontrol::UNIQUE);
    connect(m_auto_coin_selection, &Checkbox::toggled, this, &Send::onAutoSelectionToggled,
            qontrol::UNIQUE);
    connect(m_controller, &AccountController::updateCoins, this, &Send::onCoinsUpdated,
            qontrol::UNIQUE);
    connect(m_fee_toggle, &QCheckBox::toggled, this, &Send::onFeeToggled, qontrol::UNIQUE);
    connect(m_fee_value_input, &QLineEdit::textChanged, this, &Send::onProcess, qontrol::UNIQUE);
    connect(this, &Send::validationReady, this, &Send::onValidationResult, qontrol::UNIQUE);
    connect(this, &Send::signReady, this, &Send::onSignResult, qontrol::UNIQUE);
    connect(this, &Send::broadcastReady, this, &Send::onBroadcastResult, qontrol::UNIQUE);
}

void Send::view() {
    qDebug() << "Send::view()";
    m_stacked = width() < SPLIT_WIDTH;
    // Save focus state before rebuild (QPointer auto-nulls if widget is deleted)
    QPointer<QWidget> focusedWidget = QApplication::focusWidget();
    int cursorPos = 0;
    if (auto *lineEdit = qobject_cast<QLineEdit *>(focusedWidget.data())) {
        cursorPos = lineEdit->cursorPosition();
    }

    auto *oldOutputs = m_outputs_frame;
    m_outputs_frame = outputsView();
    delete oldOutputs;

    auto *oldInputs = m_inputs_frame;
    m_inputs_frame = inputsView();
    delete oldInputs;

    m_transaction_frame = transactionView();
    // Coins and payments share one column, split by a divider (no per-pane cards).
    // One trailing stretch top-packs the whole column; otherwise the extra height
    // in two-column mode is spread between items, padding the coin table.
    auto *primary = (new qontrol::Column)
                        ->push(m_inputs_frame)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new catalog::Separator)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(m_outputs_frame)
                        ->pushStretch(1); // factor > 0 so it absorbs ALL slack
    QWidget *content = nullptr;
    if (!m_stacked) {
        auto *row = (new qontrol::Row)
                        ->push(primary)
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(new catalog::Separator(catalog::Separator::Role::Vertical))
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_transaction_frame);
        // Force both columns to exactly equal width: ignore their preferred widths
        // so sizeHint differences don't skew the split, then share 50/50 via stretch.
        for (QWidget *colWidget : {static_cast<QWidget *>(primary), m_transaction_frame}) {
            auto policy = colWidget->sizePolicy();
            policy.setHorizontalPolicy(QSizePolicy::Ignored);
            colWidget->setSizePolicy(policy);
        }
        if (auto *box = qobject_cast<QBoxLayout *>(row->layout())) {
            box->setStretchFactor(primary, 1);
            box->setStretchFactor(m_transaction_frame, 1);
        }
        content = row;
    } else {
        content = (new qontrol::Column)
                      ->push(primary)
                      ->pushSpacer(resolve(Spacing::M))
                      ->push(new catalog::Separator)
                      ->pushSpacer(resolve(Spacing::M))
                      ->push(m_transaction_frame);
    }

    // Send is a two-column screen; the dashboard derives side-by-side vs stacked
    // sizing (and the width caps) from its own width, matching silent-design.
    setScreenContent(this, m_main_widget,
                     dashboard(TR("send-action-send"), content, /*two_column=*/true));

    // Restore focus state after rebuild (QPointer is null if widget was deleted)
    if (!focusedWidget.isNull()) {
        focusedWidget->setFocus();
        if (auto *lineEdit = qobject_cast<QLineEdit *>(focusedWidget.data())) {
            lineEdit->setCursorPosition(cursorPos);
        }
    }
}

void Send::resizeEvent(QResizeEvent *event) {
    qontrol::Screen::resizeEvent(event);
    // Only the one/two-column split is width-driven; rebuild when it flips.
    if ((width() < SPLIT_WIDTH) != m_stacked) {
        view();
    }
}

auto Send::outputsView() -> QWidget * {
    qDebug() << "Send::outputsView()";
    auto *oldColumn = m_outputs_column;

    m_outputs_column = new qontrol::Column;
    // Tight gap between stacked recipient cards.
    m_outputs_column->layout()->setSpacing(resolve(Spacing::XS));

    auto keys = QList<int>();
    for (auto id : m_outputs.keys()) {
        keys.push_back(id);
    }
    std::ranges::sort(keys);
    for (auto id : keys) {
        auto *output = m_outputs.value(id);
        m_outputs_column->push(output->widget());
    }
    delete oldColumn;

    // Scrollable payments list capped at 2 cards; it scrolls beyond that.
    auto *outputsScroll = new catalog::ScrollArea;
    outputsScroll->setWidget(m_outputs_column);
    int visible = keys.size() < 2 ? static_cast<int>(keys.size()) : 2;
    int outputsHeight = 0;
    for (int i = 0; i < visible; ++i) {
        outputsHeight += m_outputs.value(keys[i])->widget()->sizeHint().height();
    }
    if (visible > 1) {
        outputsHeight += (visible - 1) * resolve(Spacing::XS);
    }
    outputsScroll->setFixedHeight(outputsHeight);

    auto *title = new Label(TR("send-recipients"), LabelRole::Heading);

    // "Add payment" sits on the title row, pushed to the right edge.
    auto *titleRow = (new qontrol::Row)
                         ->pushSpacer(resolve(Padding::M))
                         ->push(title)
                         ->pushSpacer()
                         ->push(m_add_output_btn)
                         ->pushSpacer(resolve(Padding::M));

    // Fixed top/bottom rules hugging the scroll area (same kind as the table
    // header rule), shown only when the list actually scrolls (more than the 2
    // visible cards).
    const bool c_outputs_scroll = keys.size() > 2;
    auto *topRule = new catalog::Separator(catalog::Separator::Role::Horizontal);
    topRule->setStrong(true);
    topRule->setVisible(c_outputs_scroll);
    auto *bottomRule = new catalog::Separator(catalog::Separator::Role::Horizontal);
    bottomRule->setStrong(true);
    bottomRule->setVisible(c_outputs_scroll);

    // No trailing stretch here: the primary column owns the single trailing
    // stretch, so this frame stays at its content height.
    auto *col = (new qontrol::Column)
                    ->push(titleRow)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(topRule)
                    ->push(outputsScroll)
                    ->push(bottomRule);

    return col;
}

void Send::updateTransactionFlow() {
    if (m_tx_flow == nullptr) {
        return;
    }

    QList<RustCoin> selectedCoins;
    if (m_auto_coin_selection->isChecked()) {
        auto availableCoins = m_controller->getCoins();
        for (const auto &coin : availableCoins) {
            auto outpoint = QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size());
            if (m_auto_selected_outpoints.contains(outpoint)) {
                selectedCoins.append(coin);
            }
        }
    } else {
        selectedCoins = m_selected_coins;
    }

    uint64_t selectedTotal = 0;
    QList<catalog::TxFlow::InputItem> inputs;
    for (const auto &coin : selectedCoins) {
        selectedTotal += coin.value;
        catalog::TxFlow::InputItem item;
        item.weight = static_cast<double>(coin.value) / SATS;
        item.title = TR("txflow-input-n").arg(inputs.size() + 1);
        item.usd = usdAmount(coin.value);
        item.addr = QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size());
        inputs.append(item);
    }

    QList<catalog::TxFlow::OutputItem> outputs;
    uint64_t sendingTotal = 0;
    for (auto *output : m_outputs) {
        auto amount = output->amount();
        uint64_t value = amount.value_or(0);
        if (value == 0) {
            continue;
        }
        sendingTotal += value;
        catalog::TxFlow::OutputItem item;
        item.kind = catalog::TxFlow::OutputKind::Recipient;
        item.weight = static_cast<double>(value) / SATS;
        item.title = output->label().isEmpty() ? TR("txflow-output-n").arg(outputs.size() + 1)
                                               : output->label();
        item.label = output->label();
        item.usd = usdAmount(value);
        item.addr = output->address();
        outputs.append(item);
    }

    uint64_t change = selectedTotal > sendingTotal + m_estimated_fee
                          ? selectedTotal - sendingTotal - m_estimated_fee
                          : 0;
    uint64_t total = sendingTotal + m_estimated_fee;

    if (change > 0) {
        catalog::TxFlow::OutputItem item;
        item.kind = catalog::TxFlow::OutputKind::Change;
        item.weight = static_cast<double>(change) / SATS;
        item.title = TR("txflow-change-title");
        item.usd = usdAmount(change);
        item.addr = TR("txflow-change-addr");
        outputs.append(item);
    }
    if (m_estimated_fee > 0 && selectedTotal > 0) {
        catalog::TxFlow::OutputItem item;
        item.kind = catalog::TxFlow::OutputKind::Fee;
        item.weight = static_cast<double>(m_estimated_fee) / SATS;
        item.title = TR("txflow-fee-title");
        item.usd = usdAmount(m_estimated_fee);
        item.addr = TR("txflow-fee-addr");
        outputs.append(item);
    }

    m_tx_flow->setInputs(inputs);
    m_tx_flow->setOutputs(outputs);

    m_flow_inputs_eyebrow->setText(TR("txflow-inputs-count").arg(inputs.size()));
    m_flow_outputs_eyebrow->setText(TR("txflow-outputs-count").arg(outputs.size()));
    m_legend_change->setVisible(change > 0);
    m_legend_fee->setVisible(m_estimated_fee > 0 && selectedTotal > 0);

    m_sending_btc->setText(sendingTotal > 0 ? toBitcoin(sendingTotal) : QString("--"));
    m_sending_fiat->setText(sendingTotal > 0 ? usdAmount(sendingTotal) : QString());
    m_fee_btc->setText(m_estimated_fee > 0 ? toBitcoin(m_estimated_fee) : QString("--"));
    m_fee_fiat->setText(m_estimated_fee > 0 ? usdAmount(m_estimated_fee) : QString());
    m_total_btc->setText(total > 0 ? toBitcoin(total) : QString("--"));
    m_total_fiat->setText(total > 0 ? usdAmount(total) : QString());
    m_change_btc->setText(toBitcoin(change));
    m_change_fiat->setText(usdAmount(change));
    // Design hides the change row when there is no change.
    bool hasChange = change > 0;
    m_change_label->setVisible(hasChange);
    m_change_btc->setVisible(hasChange);
    m_change_fiat->setVisible(hasChange);
}

auto Send::transactionView() -> QWidget * {
    m_tx_flow = new catalog::TxFlow;

    // Eyebrow counts (left/right) + center legend, all updated in place by
    // updateTransactionFlow(); the change/fee swatches toggle visibility.
    m_flow_inputs_eyebrow = new Label(LabelRole::Caption);
    m_flow_outputs_eyebrow = new Label(LabelRole::Caption);
    m_flow_outputs_eyebrow->setAlignment(Qt::AlignRight);

    auto *legendRecipient = (new qontrol::Row)
                                ->push(new LegendSwatch(catalog::TxFlow::OutputKind::Recipient))
                                ->pushSpacer(resolve(Spacing::XS))
                                ->push(new Label(TR("txflow-recipient"), LabelRole::Caption));
    m_legend_change = (new qontrol::Row)
                          ->push(new LegendSwatch(catalog::TxFlow::OutputKind::Change))
                          ->pushSpacer(resolve(Spacing::XS))
                          ->push(new Label(TR("txflow-change"), LabelRole::Caption));
    m_legend_fee = (new qontrol::Row)
                       ->push(new LegendSwatch(catalog::TxFlow::OutputKind::Fee))
                       ->pushSpacer(resolve(Spacing::XS))
                       ->push(new Label(TR("txflow-fee"), LabelRole::Caption));
    auto *legend = (new qontrol::Row)
                       ->push(legendRecipient)
                       ->pushSpacer(resolve(Spacing::S))
                       ->push(m_legend_change)
                       ->pushSpacer(resolve(Spacing::S))
                       ->push(m_legend_fee);
    auto *flowFooter = (new qontrol::Row)
                           ->push(m_flow_inputs_eyebrow)
                           ->pushSpacer()
                           ->push(legend)
                           ->pushSpacer()
                           ->push(m_flow_outputs_eyebrow);

    auto *flowColumn =
        (new qontrol::Column)->push(m_tx_flow)->pushSpacer(resolve(Spacing::XS))->push(flowFooter);
    m_flow_card = new catalog::Card(catalog::Card::Role::Inset);
    m_flow_card->setContent(flowColumn);

    // Transaction summary as a grid so the BTC and fiat values line up in two
    // right-aligned columns. The change row is always present and toggled by
    // updateTransactionFlow() so live updates never restructure the layout.
    auto *summary = new qontrol::Grid;
    summary->spacing(resolve(Spacing::M), resolve(Spacing::XS));
    // Full width: label on the left, the two value columns pushed to the right by
    // a stretch in the middle. Columns 2 (BTC) and 3 (fiat) auto-align across rows.
    summary->columnStretch(1, 1);
    int sr = 0;
    summary->push(new catalog::Separator, sr++, 0, 1, 4);

    summary->push(new Label(TR("send-summary-sending"), LabelRole::InfoLabel), sr, 0);
    m_sending_btc = new Label("--", LabelRole::Mono);
    m_sending_btc->setAlignment(Qt::AlignRight);
    m_sending_fiat = new Label(QString(), LabelRole::Mono);
    m_sending_fiat->setAlignment(Qt::AlignRight);
    summary->push(m_sending_btc, sr, 2);
    summary->push(m_sending_fiat, sr++, 3);

    summary->push(new Label(TR("send-summary-fee"), LabelRole::InfoLabel), sr, 0);
    m_fee_btc = new Label("--", LabelRole::Mono);
    m_fee_btc->setAlignment(Qt::AlignRight);
    m_fee_fiat = new Label(QString(), LabelRole::Mono);
    m_fee_fiat->setAlignment(Qt::AlignRight);
    summary->push(m_fee_btc, sr, 2);
    summary->push(m_fee_fiat, sr++, 3);

    m_change_label = new Label(TR("send-summary-change"), LabelRole::InfoLabel);
    summary->push(m_change_label, sr, 0);
    m_change_btc = new Label("--", LabelRole::Mono);
    m_change_btc->setAlignment(Qt::AlignRight);
    m_change_fiat = new Label(QString(), LabelRole::Mono);
    m_change_fiat->setAlignment(Qt::AlignRight);
    summary->push(m_change_btc, sr, 2);
    summary->push(m_change_fiat, sr++, 3);

    summary->push(new catalog::Separator, sr++, 0, 1, 4);

    summary->push(new Label(TR("send-summary-total"), LabelRole::Heading), sr, 0);
    m_total_btc = new Label("--", LabelRole::Mono);
    m_total_btc->setAlignment(Qt::AlignRight);
    m_total_btc->setScale(1.2);
    m_total_fiat = new Label(QString(), LabelRole::Mono);
    m_total_fiat->setAlignment(Qt::AlignRight);
    m_total_fiat->setScale(1.2);
    summary->push(m_total_btc, sr, 2);
    summary->push(m_total_fiat, sr, 3);

    auto *warningRow = (new qontrol::Row)->push(m_warning_label)->pushSpacer();
    auto *actionRow =
        (new qontrol::Row)
            ->push(new Label(TR("send-fee-rate-label"), LabelRole::InfoLabel))
            ->pushSpacer(resolve(Spacing::S))
            ->push(m_fee_toggle)
            ->pushSpacer(resolve(Spacing::S))
            ->push(m_fee_value_input)
            ->push(m_fee_indicator)
            ->pushSpacer(resolve(Spacing::S))
            ->push(m_fee_label)
            ->pushSpacer(resolve(Spacing::S))
            ->push(m_fee_estimate_label)
            ->pushSpacer()
            ->push(m_clear_outputs_btn)
            ->pushSpacer(resolve(Spacing::S))
            ->push(!m_signed_tx_hex.isEmpty()
                       ? static_cast<QWidget *>(new catalog::Badge(TR("confirm-send-signed"),
                                                                   catalog::Badge::Role::Success))
                       : new QWidget)
            ->pushSpacer(resolve(Spacing::S))
            ->push(m_send_btn);

    m_send_btn->setText(m_signed_tx_hex.isEmpty() ? TR("send-action-sign")
                                                  : TR("common-broadcast"));

    auto *title = new Label(TR("send-transaction-title"), LabelRole::Heading);
    auto *col = new qontrol::Column;
    col->push(title)
        ->pushSpacer(resolve(Spacing::M))
        ->push(m_flow_card)
        ->pushStretch(1) // expanding gap: drops the summary block to the column bottom
        ->push(summary)
        ->push(actionRow) // total send sits right on top of the fee + button row
        ->pushSpacer(resolve(Spacing::S))
        ->push(warningRow);

    updateTransactionFlow(); // populate the flow + summary with the current state
    return col;
}

auto Send::inputsView() -> QWidget * {
    qDebug() << "Send::inputsView()";
    auto availableCoins = m_controller->getCoins();

    // Unparent persistent rows
    m_inputs_amounts_row->setParent(nullptr);

    bool autoMode = m_auto_coin_selection->isChecked();
    m_coin_table = new catalog::Table;
    // In auto mode the coins are picked for the user: the table is still shown
    // (and scrollable), but rows are not selectable.
    m_coin_table->setSelectionEnabled(!autoMode);
    m_coin_table->setHeaderVisible(true);
    // Show at most a 6-row window; the body scrolls beyond that.
    constexpr int c_max_visible_coins = 6;
    const int c_coin_count = static_cast<int>(availableCoins.size());
    const int c_visible_rows =
        c_coin_count < c_max_visible_coins ? c_coin_count : c_max_visible_coins;
    m_coin_table->setBodyHeight(c_visible_rows * metric::TABLE_ROW_HEIGHT);

    // When the body scrolls, its scrollbar insets the row values on the right.
    // Add the same gutter to the amounts row so the total selected value stays
    // aligned with the value column.
    const bool c_has_scrollbar = c_coin_count > c_max_visible_coins;
    const int c_amounts_right_pad =
        resolve(Padding::S) + (c_has_scrollbar ? catalog::ScrollArea::scrollBarThickness() : 0);
    m_inputs_amounts_row->layout()->setContentsMargins(resolve(Padding::S), 0, c_amounts_right_pad,
                                                       0);

    m_coin_table->addColumn({.title = TR("coins-type"), .width = resolve(Size::XXS)});
    m_coin_table->addColumn(
        {.title = TR("coins-outpoint"), .width = resolve(Size::M), .role = DisplayRole::Outpoint});
    m_coin_table->addColumn({.title = TR("coins-label"), .grow = true});
    m_coin_table->addColumn({.title = TR("coins-value"),
                             .width = resolve(Size::S),
                             .alignment = Qt::AlignRight,
                             .headerAlignment = Qt::AlignRight,
                             .role = DisplayRole::Amount});

    for (const auto &coin : availableCoins) {
        QString outpoint = QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size());
        QString typeStr = QString::fromUtf8(coin.account_type.data(), coin.account_type.size());
        QString label = QString::fromUtf8(coin.label.data(), coin.label.size());
        // Design shows a placeholder for empty labels (hyphen, not an em dash).
        QString labelCell = label.isEmpty() ? QStringLiteral("-") : label;
        m_coin_table->addRow(
            outpoint, {typeStr, shortenOutpoint(outpoint), labelCell, toBitcoin(coin.value)});
    }
    connect(m_coin_table, &catalog::Table::rowSelectionChanged, this, &Send::onCoinSelectionChanged,
            qontrol::UNIQUE);
    updateCoinCheckboxes();

    m_auto_coin_selection->setParent(nullptr);
    m_inputs_title_label->setParent(nullptr);
    m_clear_inputs_btn->setParent(nullptr);
    m_clear_inputs_btn->setVisible(!autoMode); // Clear sits in the coins header, manual only

    auto *titleRow = (new qontrol::Row)
                         ->pushSpacer(resolve(Padding::M))
                         ->push(m_inputs_title_label)
                         ->pushSpacer()
                         ->push(m_clear_inputs_btn)
                         ->pushSpacer(resolve(Spacing::XS))
                         ->push(m_auto_coin_selection)
                         ->pushSpacer(resolve(Padding::M));

    // Update total and minimum displays
    onUpdateInputsTotal();

    auto *col = new qontrol::Column;
    // The coin table is always shown and scrollable, in both auto and manual
    // selection modes (auto mode just makes the rows non-selectable).
    col->push(titleRow)->pushSpacer(resolve(Spacing::XS))->push(m_coin_table);
    col->pushSpacer(resolve(Spacing::XS))->push(m_inputs_amounts_row);
    // No trailing stretch here: in two-column mode it would inflate and push the
    // separator far below the table. The outputs frame carries the trailing stretch.

    return col;
}

void Send::onAddOutput() {
    qDebug() << "Send::onAddOutput()";
    auto *output = new OutputW(this, m_output_id);
    if (m_outputs.empty()) {
        output->setDeletable(false);
    } else {
        for (auto &out : m_outputs) {
            out->setDeletable(true);
        }
    }
    for (auto *outp : m_outputs) {
        if (outp->isMax()) {
            output->enableMax(false);
            break;
        }
    }
    m_outputs.insert(m_output_id, output);
    m_outputs_column->push(output->widget());
    m_output_id++;
    onProcess();
    view();
}

void Send::onDeleteOutput(int id) {
    qDebug() << "Send::onDeleteOutput()" << id;
    auto *output = m_outputs.take(id);
    if (output->isMax()) {
        for (auto *outp : m_outputs) {
            outp->enableMax(true);
        }
    }
    delete output->widget();
    delete output;
    if (m_outputs.size() == 1) {
        auto *outp = m_outputs.value(m_outputs.keys().first());
        outp->setDeletable(false);
        if (!outp->isMax()) {
            outp->enableMax(true);
        }
    }
    onProcess();
    view();
}

void Send::onOutputSetMax(int id) {
    qDebug() << "Send::onOutputSetMax()" << id;
    if (m_outputs.value(id)->isMax()) {
        for (auto &key : m_outputs.keys()) {
            if (key != id) {
                m_outputs.value(key)->enableMax(false);
            }
        }
    } else {
        for (auto &key : m_outputs.keys()) {
            m_outputs.value(key)->enableMax(true);
        }
    }
    onProcess();
}

void Send::onSetBroadcastable(bool broadcastable) {
    qDebug() << "Send::onSetBroadcastable()" << broadcastable;
    m_broadcastable = broadcastable;
    m_send_btn->setEnabled(broadcastable);
}

void Send::onClearOutputs() {
    qDebug() << "Send::onClearOutputs()";
    for (auto *outp : m_outputs) {
        delete outp;
    }
    m_outputs.clear();
    onAddOutput();
    onProcess();
    view();
}

void Send::onClearInputs() {
    qDebug() << "Send::onClearInputs()";
    m_selected_coins.clear();
    onProcess();
    view();
}

void Send::onCoinSelectionChanged(const QString &row_id, bool selected) {
    auto availableCoins = m_controller->getCoins();
    if (selected) {
        for (const auto &coin : availableCoins) {
            if (QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size()) == row_id) {
                bool alreadySelected = false;
                for (const auto &selectedCoin : m_selected_coins) {
                    if (selectedCoin.outpoint == coin.outpoint) {
                        alreadySelected = true;
                        break;
                    }
                }
                if (!alreadySelected) {
                    m_selected_coins.append(coin);
                }
                break;
            }
        }
    } else {
        for (int i = 0; i < m_selected_coins.size(); ++i) {
            if (QString::fromUtf8(m_selected_coins[i].outpoint.data(),
                                  m_selected_coins[i].outpoint.size()) == row_id) {
                m_selected_coins.removeAt(i);
                break;
            }
        }
    }
    onProcess();
}

void Send::onCoinToggled() {
    qDebug() << "Send::onCoinToggled()";
    auto *checkbox = qobject_cast<Checkbox *>(sender());
    if (checkbox == nullptr) {
        return;
    }

    QString outpoint = checkbox->property("outpoint").toString();
    auto availableCoins = m_controller->getCoins();

    if (checkbox->isChecked()) {
        // Add to m_selected_coins if not already present
        for (const auto &coin : availableCoins) {
            if (QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size()) == outpoint) {
                bool alreadySelected = false;
                for (const auto &selected : m_selected_coins) {
                    if (selected.outpoint == coin.outpoint) {
                        alreadySelected = true;
                        break;
                    }
                }
                if (!alreadySelected) {
                    m_selected_coins.append(coin);
                }
                break;
            }
        }
    } else {
        // Remove from m_selected_coins
        for (int i = 0; i < m_selected_coins.size(); ++i) {
            if (QString::fromUtf8(m_selected_coins[i].outpoint.data(),
                                  m_selected_coins[i].outpoint.size()) == outpoint) {
                m_selected_coins.removeAt(i);
                break;
            }
        }
    }
    onProcess();
}

void Send::onAutoSelectionToggled() {
    qDebug() << "Send::onAutoSelectionToggled()";
    if (!m_auto_coin_selection->isChecked()) {
        // Switching to manual: populate m_selected_coins from last auto selection
        onUpdateSelectedCoinsFromSimulation();
    }
    onProcess();
    view();
}

void Send::onCoinsUpdated(CoinState state) {
    qDebug() << "Send::onCoinsUpdated() confirmed:" << state.confirmed_count
             << "unconfirmed:" << state.unconfirmed_count;
    // Only rebuild if coins actually changed
    bool changed = (state.confirmed_count != m_last_coin_state.confirmed_count ||
                    state.unconfirmed_count != m_last_coin_state.unconfirmed_count);

    if (!changed) {
        return;
    }

    m_last_coin_state = state;

    // Rebuild the inputs view to show new/removed coins
    view();

    // If auto coin selection is enabled AND outputs are valid, re-run simulation
    if (m_auto_coin_selection->isChecked()) {
        auto txTemp = txTemplate();
        if (txTemp.has_value()) {
            onProcess();
        }
    }
}

void Send::onUpdateSelectedCoinsFromSimulation() {
    qDebug() << "Send::onUpdateSelectedCoinsFromSimulation()";
    m_selected_coins.clear();
    auto availableCoins = m_controller->getCoins();
    for (const auto &coin : availableCoins) {
        QString op = QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size());
        if (m_auto_selected_outpoints.contains(op)) {
            m_selected_coins.append(coin);
        }
    }
}

void Send::onUpdateInputsTotal() {
    qDebug() << "Send::onUpdateInputsTotal()";
    // Calculate total selected based on mode
    uint64_t totalSelected = 0;

    if (m_auto_coin_selection->isChecked()) {
        // In auto mode, calculate from auto-selected outpoints
        auto availableCoins = m_controller->getCoins();
        for (const auto &coin : availableCoins) {
            QString op = QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size());
            if (m_auto_selected_outpoints.contains(op)) {
                totalSelected += coin.value;
            }
        }
    } else {
        // In manual mode, calculate from m_selected_coins
        for (const auto &coin : m_selected_coins) {
            totalSelected += coin.value;
        }
    }

    // Minimum required to cover the (non-max) outputs.
    uint64_t minRequired = 0;
    for (auto *out : m_outputs) {
        if (!out->isMax()) {
            auto amt = out->amount();
            if (amt.has_value()) {
                minRequired += amt.value();
            }
        }
    }

    uint64_t remaining = minRequired > totalSelected ? minRequired - totalSelected : 0;
    m_inputs_min_value->setText(QString::number(static_cast<double>(remaining) / SATS, 'f', 8) +
                                " BTC");
    m_inputs_total_value->setText(
        QString::number(static_cast<double>(totalSelected) / SATS, 'f', 8) + " BTC");
    // Always visible, including in auto-selection mode.
    m_inputs_amounts_row->setVisible(true);
}

void Send::updateCoinCheckboxes() {
    qDebug() << "Send::updateCoinCheckboxes()";
    bool autoMode = m_auto_coin_selection->isChecked();

    if (m_coin_table != nullptr) {
        QStringList selectedIds;
        if (autoMode) {
            selectedIds = m_auto_selected_outpoints;
        } else {
            for (const auto &coin : m_selected_coins) {
                selectedIds.append(QString::fromUtf8(coin.outpoint.data(), coin.outpoint.size()));
            }
        }
        m_coin_table->setSelectedIds(selectedIds);
        // Keep the table enabled in auto mode so it stays scrollable; rows are
        // already made non-selectable via setSelectionEnabled(!autoMode).
    }
    updateInputsTitle();
}

void Send::updateInputsTitle() {
    qDebug() << "Send::updateInputsTitle()";
    int count = 0;
    if (m_auto_coin_selection->isChecked()) {
        count = m_auto_selected_outpoints.size();
    } else {
        count = m_selected_coins.size();
    }

    if (count == 1) {
        m_inputs_title_label->setText(TR("send-inputs-selected-one"));
    } else if (count > 1) {
        m_inputs_title_label->setText(TR("send-inputs-selected-many").arg(count));
    } else {
        m_inputs_title_label->setText(TR("send-inputs"));
    }
}

auto OutputW::address() -> QString {
    return m_address_input->text();
}

auto OutputW::amount() -> std::optional<uint64_t> {
    auto amountStr = m_amount_input->text();
    bool ok = false;
    auto amountBtc = amountStr.toDouble(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(amountBtc * SATS);
}

void OutputW::syncUsdFromBtc() {
    bool ok = false;
    double btc = m_amount_input->text().toDouble(&ok);
    QSignalBlocker blocker(m_amount_usd_input);
    if (!ok || btc <= 0.0) {
        m_amount_usd_input->setText(QString());
        return;
    }
    m_amount_usd_input->setText(QString::number(btc * USD_RATE, 'f', 2));
}

void OutputW::syncBtcFromUsd() {
    auto btcText = btcFromUsd(m_amount_usd_input->text());
    QSignalBlocker blocker(m_amount_input);
    m_amount_input->setText(btcText);
}

auto OutputW::amountInput() -> QWidget * {
    return m_amount_input;
}

auto OutputW::usdInput() -> QWidget * {
    return m_amount_usd_input;
}

auto OutputW::label() -> QString {
    return m_label_input->text();
}

void OutputW::updateAddressValidation() {
    QString addr = m_address_input->text();
    if (addr.isEmpty()) {
        setValidationIndicator(m_address_indicator, addr, false);
    } else {
        auto result = ::validate_address(rust::String(addr.toStdString()));
        setValidationIndicator(m_address_indicator, addr, result.empty());
    }
}

void OutputW::updateAmountValidation() {
    QString text = m_amount_input->text();
    if (text.isEmpty()) {
        setValidationIndicator(m_amount_indicator, text, false);
    } else {
        bool ok = false;
        double val = text.toDouble(&ok);
        setValidationIndicator(m_amount_indicator, text, ok && val > 0);
    }
}

auto Send::txTemplate() -> std::optional<TransactionTemplate> {
    qDebug() << "Send::txTemplate()";
    auto txTemplate = TransactionTemplate();
    bool ok = false;

    // Fee handling: toggle ON = sats/vb (rate), toggle OFF = sats (absolute)
    auto feeValue = m_fee_value_input->text().toDouble(&ok);
    if (!ok || feeValue <= 0) {
        return std::nullopt;
    }

    if (m_fee_toggle->isChecked()) {
        // Fee rate mode (sats/vb)
        txTemplate.fee_rate = feeValue;
        txTemplate.fee = 0;
    } else {
        // Absolute fee mode (sats)
        txTemplate.fee = static_cast<uint64_t>(feeValue);
        txTemplate.fee_rate = 1.0; // Fallback, not used when fee > 0
    }

    // Outputs
    for (auto *out : m_outputs) {
        auto output = Output();
        output.address = rust::String(out->address().toStdString());

        if (out->isMax()) {
            output.max = true;
            output.amount = 0; // Amount is ignored when max is true
        } else {
            auto amount = out->amount();
            if (!amount.has_value()) {
                return std::nullopt;
            }
            output.amount = amount.value();
            output.max = false;
        }

        output.label = rust::String(out->label().toStdString());
        txTemplate.outputs.push_back(output);
    }

    // Add selected coins as input outpoints only when manual selection is active
    if (!m_auto_coin_selection->isChecked()) {
        for (const auto &coin : m_selected_coins) {
            txTemplate.input_outpoints.push_back(rust::String(std::string(coin.outpoint)));
        }
    }
    // When auto is checked, leave input_outpoints empty -> Rust auto-selects

    return txTemplate;
}

void Send::onSetSpendable(bool spendable) {
    qDebug() << "Send::onSetSpendable()" << spendable;
    m_send_btn->setEnabled(spendable);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void Send::onUpdateOutputValidations() {
    qDebug() << "Send::onUpdateOutputValidations()";
    for (auto *out : m_outputs) {
        out->updateAddressValidation();
        out->updateAmountValidation();
    }
}

void Send::onUpdateFeeValidation() {
    qDebug() << "Send::onUpdateFeeValidation()";
    QString text = m_fee_value_input->text();
    if (text.isEmpty()) {
        setValidationIndicator(m_fee_indicator, text, false);
    } else {
        bool ok = false;
        double val = text.toDouble(&ok);
        setValidationIndicator(m_fee_indicator, text, ok && val > 0);
    }
}

void Send::onProcess() {
    qDebug() << "Send::onProcess()";
    if (!m_signed_tx_hex.isEmpty()) {
        m_signed_tx_hex.clear();
        m_warning_label->clear();
        m_warning_label->setVisible(false);
    }
    auto *focused = QApplication::focusWidget();
    for (auto *out : m_outputs) {
        if (focused == out->usdInput()) {
            out->syncBtcFromUsd();
        } else {
            out->syncUsdFromBtc();
        }
    }
    onUpdateOutputValidations();
    onUpdateFeeValidation();
    onUpdateInputsTotal();

    auto txTemp = txTemplate();
    if (!txTemp.has_value()) {
        onSetSpendable(false);
        onSetBroadcastable(false);
        m_fee_estimate_label->setVisible(false);
        // Clear auto selection when outputs are invalid
        if (m_auto_coin_selection->isChecked() && !m_auto_selected_outpoints.isEmpty()) {
            m_auto_selected_outpoints.clear();
            updateCoinCheckboxes();
            onUpdateInputsTotal();
        }
        updateTransactionFlow();
        return;
    }

    auto simu = m_controller->simulateTx(txTemp.value());

    if (simu.error.empty()) {
        if (simu.is_valid) {
            onSetSpendable(true);
            m_warning_label->setVisible(false);

            // Display estimated fee
            m_estimated_fee = simu.fee;
            m_fee_estimate_label->setText(TR("send-fee-estimate").arg(toBitcoin(simu.fee)));
            m_fee_estimate_label->setVisible(true);

            // Fill in estimated max amount for MAX outputs
            for (auto *out : m_outputs) {
                if (out->isMax()) {
                    uint64_t nonMaxTotal = 0;
                    for (auto *other : m_outputs) {
                        if (!other->isMax()) {
                            auto amt = other->amount();
                            if (amt.has_value()) {
                                nonMaxTotal += amt.value();
                            }
                        }
                    }
                    uint64_t maxAmount =
                        simu.output_total > nonMaxTotal ? simu.output_total - nonMaxTotal : 0;
                    out->setMaxAmount(maxAmount);
                    break;
                }
            }

            // Store auto-selected outpoints for display when auto mode is active
            if (m_auto_coin_selection->isChecked()) {
                QStringList newSelection;
                for (const auto &op : simu.selected_outpoints) {
                    QString opStr = QString::fromUtf8(op.data(), op.size());
                    newSelection.append(opStr);
                }
                m_auto_selected_outpoints = newSelection;
                updateCoinCheckboxes();
                onUpdateInputsTotal();
            }
        } else {
            onSetSpendable(false);
            m_estimated_fee = 0;
            m_fee_estimate_label->setVisible(false);
            m_warning_label->setText(TR("send-transaction-invalid"));
            m_warning_label->setVisible(true);
        }
    } else {
        onSetSpendable(false);
        onSetBroadcastable(false);
        m_estimated_fee = 0;
        m_fee_estimate_label->setVisible(false);
        auto rawError = QString::fromStdString(std::string(simu.error.c_str()));
        m_warning_label->setText(mapBackendErrorSummary(rawError));
        m_warning_label->setVisible(true);
    }

    // Reflect the updated inputs/outputs/fee in the graphical flow as the user types.
    updateTransactionFlow();
}

void Send::onSendTransaction() {
    qDebug() << "Send::onSendTransaction()";
    if (!m_signed_tx_hex.isEmpty()) {
        onBroadcastConfirmed();
        return;
    }

    auto txTemp = txTemplate();
    if (!txTemp.has_value()) {
        AppController::execModal(
            new qontrol::Modal(TR("common-error"), TR("send-invalid-transaction-template")));
        return;
    }

    auto &account = m_controller->getAccount();
    if (!account.has_value()) {
        AppController::execModal(
            new qontrol::Modal(TR("common-error"), TR("send-no-account-loaded")));
        return;
    }

    // Prepare transaction on main thread (pure computation)
    auto psbt = account.value()->prepare_transaction(txTemp.value());
    if (!psbt->is_ok()) {
        auto error = QString::fromStdString(std::string(psbt->get_psbt_error().c_str()));
        auto text = mapBackendErrorSummary(error) + "\n\n" + formatBackendErrorDetails(error);
        AppController::execModal(new qontrol::Modal(TR("send-prepare-failed"), text));
        return;
    }

    // Store psbt and tx template for use in validation / onSendConfirmed / logging
    m_psbt_result = std::make_optional(std::move(psbt));
    m_tx_template = txTemp;

    // Run PSBT validation on background thread before showing confirm modal
    m_send_btn->setEnabled(false);
    auto *thread = QThread::create([this]() {
        auto result =
            m_controller->getAccount().value()->validate_before_sign(*m_psbt_result.value());
        emit validationReady(result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void Send::onValidationResult(PsbtValidation result) {
    qDebug() << "Send::onValidationResult() is_ok:" << result.is_ok
             << "is_valid:" << result.is_valid;

    m_send_btn->setEnabled(true);

    if (!result.is_ok) {
        // Connection/protocol error: warn but don't block
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        auto text = mapBackendErrorSummary(error) + "\n\n" + formatBackendErrorDetails(error);
        AppController::execModal(new qontrol::Modal(TR("send-validation-error"), text));
        // Fall through to show ConfirmSend anyway
    } else if (result.spent_input_count > 0) {
        // Spent inputs: block the transaction
        auto issues = QString::fromStdString(std::string(result.issues.c_str()));
        AppController::execModal(
            new qontrol::Modal(TR("common-error"), TR("send-inputs-already-spent").arg(issues)));
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        return;
    } else if (result.reused_output_count > 0) {
        // Address reuse: warn and ask user via accept/reject modal
        if (!showReuseWarning(result)) {
            return;
        }
    }

    if (!m_psbt_result.has_value() || !m_tx_template.has_value()) {
        return;
    }

    showSignerStep();
}

auto Send::showReuseWarning(PsbtValidation result) -> bool {
    auto issues = QString::fromStdString(std::string(result.issues.c_str()));
    auto *modal = new qontrol::Modal();
    modal->setWindowTitle(TR("send-address-reuse-warning"));
    auto *label = new Label(issues + "\n\n" + TR("send-proceed-anyway"));
    label->setWordWrap(true);
    auto *proceedBtn = new Button(TR("common-proceed"));
    auto *cancelBtn = new Button(TR("common-cancel"));
    connect(proceedBtn, &QPushButton::clicked, modal, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, modal, &QDialog::reject);
    auto *btnRow = (new qontrol::Row)
                       ->pushSpacer()
                       ->push(cancelBtn)
                       ->pushSpacer()
                       ->push(proceedBtn)
                       ->pushSpacer();
    auto *col = (new qontrol::Column)
                    ->push(label)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(btnRow)
                    ->pushSpacer();
    modal->setMainWidget(margin(col, resolve(Spacing::S)));
    int dialogResult = modal->exec();
    delete modal;
    if (dialogResult != QDialog::Accepted) {
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        return false;
    }
    return true;
}

void Send::showSignerStep() {
    auto *picker = new modal::SelectSigner(TR("send-select-signer"));
    int dialogResult = picker->exec();
    auto signerName = picker->selectedName();
    auto signerKind = picker->selectedKind();
    delete picker;
    if (dialogResult != QDialog::Accepted) {
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        return;
    }
    auto *status = new catalog::ModalStatus;
    status->setState(catalog::ModalStatus::State::Signing);
    status->setSubtitle(signerKind == "sd" ? TR("confirm-send-sd-step")
                                           : TR("confirm-send-device-step").arg(signerName));
    auto *continueBtn = new Button(TR("common-proceed"), ButtonRole::Primary);
    auto *cancelBtn = new Button(TR("common-cancel"));
    auto *stepModal = new qontrol::Modal;
    stepModal->setWindowTitle(signerName);
    connect(continueBtn, &QPushButton::clicked, stepModal, &QDialog::accept, qontrol::UNIQUE);
    connect(cancelBtn, &QPushButton::clicked, stepModal, &QDialog::reject, qontrol::UNIQUE);
    auto *btnRow = (new qontrol::Row)
                       ->pushSpacer()
                       ->push(cancelBtn)
                       ->pushSpacer(resolve(Spacing::XS))
                       ->push(continueBtn)
                       ->pushSpacer();
    stepModal->setMainWidget(margin((new qontrol::Column)
                                        ->push(status)
                                        ->pushSpacer(resolve(Spacing::M))
                                        ->push(btnRow)
                                        ->pushSpacer()));
    int stepResult = stepModal->exec();
    delete stepModal;
    if (stepResult != QDialog::Accepted) {
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        return;
    }
    onSendConfirmed();
}

void Send::onSendConfirmed() {
    qDebug() << "Send::onSendConfirmed()";
    auto &account = m_controller->getAccount();
    if (!account.has_value() || !m_psbt_result.has_value()) {
        AppController::execModal(
            new qontrol::Modal(TR("common-error"), TR("send-transaction-state-lost")));
        return;
    }

    // Sign on background thread (future-proofed for hardware signing devices)
    auto *thread = QThread::create([this]() {
        auto result = m_controller->getAccount().value()->sign_transaction(*m_psbt_result.value());
        emit signReady(result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void Send::onSignResult(TxResult result) {
    qDebug() << "Send::onSignResult() ok:" << result.is_ok;
    if (!result.is_ok) {
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        m_psbt_result = std::nullopt;
        m_tx_template = std::nullopt;
        AppController::execModal(
            new qontrol::Modal(TR("common-error"), TR("send-signing-failed") + "\n\n" +
                                                       formatBackendErrorDetails(error)));
        return;
    }

    m_signed_tx_hex = QString::fromStdString(std::string(result.value.c_str()));
    m_psbt_result = std::nullopt;
    m_warning_label->setText(TR("confirm-send-signed"));
    m_warning_label->setVisible(true);
    view();
}

void Send::onBroadcastConfirmed() {
    m_send_btn->setEnabled(false);
    m_warning_label->setText(TR("confirm-send-broadcasting"));
    m_warning_label->setVisible(true);
    auto signedHex = m_signed_tx_hex.toStdString();
    auto *thread = QThread::create([this, signedHex]() {
        auto broadcastResult =
            m_controller->getAccount().value()->broadcast_transaction(rust::String(signedHex));
        emit broadcastReady(broadcastResult);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void Send::onBroadcastResult(TxResult result) {
    qDebug() << "Send::onBroadcastResult() ok:" << result.is_ok;
    m_send_btn->setEnabled(true);

    if (!result.is_ok && m_tx_template.has_value() && !m_signed_tx_hex.isEmpty()) {
        auto &account = m_controller->getAccount();
        if (account.has_value()) {
            account.value()->log_failed_broadcast(m_tx_template.value(),
                                                  m_signed_tx_hex.toStdString());
        }
    }

    m_signed_tx_hex.clear();
    m_tx_template = std::nullopt;

    if (result.is_ok) {
        auto txid = QString::fromStdString(std::string(result.value.c_str()));
        AppController::execModal(new qontrol::Modal(TR("confirm-transaction-title"),
                                                    TR("confirm-send-success").arg(txid)));
    } else {
        auto error = QString::fromStdString(std::string(result.error.c_str()));
        AppController::execModal(
            new qontrol::Modal(TR("common-error"), TR("confirm-send-failed").arg(error)));
    }

    // Refresh coin state
    m_controller->pollCoins();

    if (result.is_ok) {
        onClearOutputs();
        onClearInputs();
    }
}

void Send::onOutputDeleteClicked() {
    qDebug() << "Send::onOutputDeleteClicked()";
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (btn != nullptr) {
        int id = btn->property("outputId").toInt();
        onDeleteOutput(id);
    }
}

void Send::onOutputMaxToggled() {
    qDebug() << "Send::onOutputMaxToggled()";
    auto *checkbox = qobject_cast<Checkbox *>(sender());
    if (checkbox != nullptr) {
        int id = checkbox->property("outputId").toInt();
        auto *output = m_outputs.value(id);
        if (output != nullptr) {
            output->setMaxMode(checkbox->isChecked());
        }
        onOutputSetMax(id);
    }
}

} // namespace view
