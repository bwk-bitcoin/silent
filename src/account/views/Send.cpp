#include "Send.h"
#include "account/Money.h"
#include "account/RequestId.h"
#include "account/SendSigning.h"
#include "catalog/Button.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Display.h"
#include "catalog/display/Label.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/Toggle.h"
#include "catalog/panels/send/TxFlow.h"
#include "i18n/Tr.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/transaction.h"
#include "interfaces/host.h"
#include "theme/Palette.h"
#include "views/modals/ScanAddress.h"
#include "views/utils.h"
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDebug>
#include <QDoubleValidator>
#include <QEvent>
#include <QIntValidator>
#include <QLocale>
#include <QResizeEvent>
#include <QSignalBlocker>
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

namespace account::view {

namespace {

auto moneyLocale(const QLineEdit &field) -> QLocale {
    return field.validator() != nullptr ? field.validator()->locale() : field.locale();
}

auto localizedMoneyText(const QLineEdit &field, QString text) -> QString {
    text.replace(QStringLiteral("."), moneyLocale(field).decimalPoint());
    return text;
}

auto normalizedMoneyText(const QLineEdit &field) -> std::optional<QString> {
    auto locale = moneyLocale(field);
    bool validSyntax = false;
    // Qt checks localized syntax only; Rust interprets the monetary value.
    static_cast<void>(locale.toDouble(field.text(), &validSyntax));
    if (!validSyntax) {
        return std::nullopt;
    }
    auto text = field.text();
    text.remove(locale.groupSeparator());
    text.replace(locale.decimalPoint(), QStringLiteral("."));
    for (auto &character : text) {
        auto digit = character.digitValue();
        if (digit >= 0) {
            character = QChar(static_cast<char16_t>(u'0' + digit));
        }
    }
    return text;
}

// The fee inputs and the step's buttons on one line, the buttons right-aligned on a line
// of their own under the fee inputs when one line cannot hold both at their full width.
class ActionRow : public QWidget {
public:
    ActionRow(QWidget *fee_inputs, QWidget *buttons)
        : m_fee_inputs(fee_inputs),
          m_buttons(buttons) {
        auto *box = new QBoxLayout(QBoxLayout::LeftToRight, this);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(resolve(Spacing::S));
        box->addWidget(fee_inputs, 0, Qt::AlignLeft | Qt::AlignBottom);
        box->addWidget(buttons, 1, Qt::AlignBottom);
    }

protected:
    auto event(QEvent *event) -> bool override {
        // A fee input or a button shown or hidden changes what one line needs.
        if (event->type() == QEvent::LayoutRequest) {
            fit();
        }
        return QWidget::event(event);
    }

    void resizeEvent(QResizeEvent *event) override {
        fit();
        QWidget::resizeEvent(event);
    }

    void fit() {
        auto *box = static_cast<QBoxLayout *>(layout());
        int oneLine =
            m_fee_inputs->sizeHint().width() + box->spacing() + m_buttons->sizeHint().width();
        auto direction = oneLine <= width() ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
        if (box->direction() != direction) {
            box->setDirection(direction);
        }
    }

private:
    QWidget *m_fee_inputs = nullptr;
    QWidget *m_buttons = nullptr;
};

auto fiatAmount(uint64_t sats) -> QString {
    auto price = Host::get()->selectedPrice();
    if (!price.rate.has_value()) {
        return {};
    }
    double btc = static_cast<double>(sats) / SATS;
    return QStringLiteral("~ %1 %2").arg(QString::number(btc * price.rate.value(), 'f', 2),
                                         price.currency);
}

} // namespace

using catalog::Checkbox;
using catalog::DisplayRole;
using catalog::Input;
using catalog::InputRole;
using catalog::Label;
using catalog::LabelRole;
using catalog::Toggle;

static void setValidationIndicator(catalog::ValidatedInput *field, const QString &text,
                                   bool valid) {
    if (text.isEmpty()) {
        field->setState(catalog::ValidationMark::State::None);
        return;
    }
    field->setValid(valid);
}

OutputW::OutputW(Send *screen, int id) {
    m_address_field = new catalog::ValidatedInput(InputRole::Mono);
    m_address_input = m_address_field->input();
    m_address_input->setPlaceholderText(TR("send-placeholder-address"));

    m_delete_btn = catalog::btnRemovePayment();
    m_scan_btn = catalog::btnScanAddress();

    m_amount_field = new catalog::ValidatedInput(InputRole::Mono);
    m_amount_input = m_amount_field->input();
    m_amount_field->setWidth(Size::S);
    m_amount_field->setFixedWidth(resolve(Size::S));
    m_amount_input->setPlaceholderText(TR("send-placeholder-amount"));
    auto *btcValidator = new QDoubleValidator(0.00000001, 21000000.0, 8, m_amount_input);
    btcValidator->setNotation(QDoubleValidator::StandardNotation);
    m_amount_input->setValidator(btcValidator);
    m_amount_fiat_input = new Input(InputRole::Mono);
    m_amount_fiat_input->setWidth(Size::S);
    m_amount_fiat_input->setPlaceholderText(QStringLiteral("0.00"));
    auto *fiatValidator = new QDoubleValidator(m_amount_fiat_input);
    fiatValidator->setBottom(0);
    fiatValidator->setDecimals(2);
    fiatValidator->setNotation(QDoubleValidator::StandardNotation);
    fiatValidator->setLocale(QLocale::c());
    m_amount_fiat_input->setValidator(fiatValidator);
    m_amount_fiat_input->setProperty("outputId", id);

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
    QObject::connect(m_amount_fiat_input, &QLineEdit::textEdited, screen, &Send::onFiatEdited,
                     qontrol::UNIQUE);
    // The label feeds the recipient's on-ribbon text in the flow diagram.
    QObject::connect(m_label_input, &QLineEdit::textChanged, screen, &Send::onProcess,
                     qontrol::UNIQUE);

    // Each value is glued to its unit (input+BTC, fiat+currency, checkbox+MAX) with a
    // tiny gap; the groups are separated by a normal gap.
    auto *btcGroup = (new qontrol::Row)
                         ->spacing(resolve(Spacing::XS))
                         ->push(m_amount_field)
                         ->push(new Label("BTC", LabelRole::Subheading));

    m_fiat_code = new Label(QString(), LabelRole::Subheading);
    m_fiat_group = (new qontrol::Row)
                       ->spacing(resolve(Spacing::XS))
                       ->push(m_amount_fiat_input)
                       ->push(m_fiat_code);

    auto *maxGroup =
        (new qontrol::Row)->spacing(resolve(Spacing::XS))->push(m_max)->push(m_max_label);

    auto *amountRow = (new qontrol::Row)
                          ->spacing(resolve(Spacing::S))
                          ->push(btcGroup)
                          ->push(m_fiat_group)
                          ->push(maxGroup)
                          ->pushSpacer();

    // The scan button takes its width from the address, which grows to fill the row.
    auto *addressRow = (new qontrol::Row)
                           ->spacing(resolve(Spacing::XS))
                           ->push(m_scan_btn)
                           ->push(m_address_field, 1);

    auto *fields = (new catalog::FieldGroup(catalog::FieldGroup::Labels::Payment))
                       ->addField(TR("send-address"), addressRow)
                       ->addField(TR("send-label"), m_label_input)
                       ->addField(TR("send-amount"), amountRow);

    auto *card = new catalog::Card;
    card->setContent(fields);
    card->setCorner(m_delete_btn);
    m_widget = card;

    m_delete_btn->setProperty("outputId", id);
    QObject::connect(m_delete_btn, &QPushButton::clicked, screen, &Send::onOutputDeleteClicked,
                     qontrol::UNIQUE);
    m_scan_btn->setProperty("outputId", id);
    QObject::connect(m_scan_btn, &QPushButton::clicked, screen, &Send::onScanAddressClicked,
                     qontrol::UNIQUE);

    m_max->setProperty("outputId", id);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QObject::connect(m_max, &QCheckBox::checkStateChanged, screen, &Send::onOutputMaxToggled,
                     qontrol::UNIQUE);
#else
    QObject::connect(m_max, &QCheckBox::stateChanged, screen, &Send::onOutputMaxToggled,
                     qontrol::UNIQUE);
#endif
    syncFiatFromBtc();
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
    m_amount_fiat_input->setEnabled(!max);
    if (max) {
        m_amount_input->setText(TR("send-max"));
        setValidationIndicator(m_amount_field, "", false);
    } else {
        if (m_amount_input->text() == TR("send-max")) {
            m_amount_input->setText("");
        }
        m_amount_input->setPlaceholderText(TR("send-placeholder-amount"));
    }
}

void OutputW::setMaxAmount(uint64_t sats) {
    // Resolved MAX text is presentation, not another payment edit.
    QSignalBlocker blocker(m_amount_input);
    m_amount_input->setText(localizedMoneyText(*m_amount_input, toBitcoin(sats, false)));
    updateAmountValidation();
    syncFiatFromBtc();
}

void OutputW::setLabel(const QString &label) {
    m_label_input->setText(label);
}

void OutputW::setFrozenAmount(std::optional<uint64_t> amount) {
    m_frozen_amount = amount;
    if (amount.has_value()) {
        setMaxAmount(amount.value());
    }
}

void Send::restoreTransaction(const backend::TransactionTemplate &transaction,
                              const QList<backend::Coin> &inputs, const QString &warning) {
    // Lock before setting fields: textChanged must never craft a restored transaction.
    m_locked = true;
    m_restored = true;
    m_preview.reset();
    m_selection_request = 0;
    m_fee_mode = FeeMode::Manual;
    m_fee_mode_picker->setCurrentIndex(static_cast<int>(m_fee_mode));
    for (auto *output : m_outputs) {
        delete output->widget();
        delete output;
    }
    m_outputs.clear();
    for (const auto &payment : transaction.outputs) {
        auto *output = new OutputW(this, m_output_id++);
        output->setAddress(payment.address);
        output->setLabel(payment.label);
        output->setFrozenAmount(payment.amount);
        output->syncFiatFromBtc();
        output->setDeletable(false);
        output->enableMax(false);
        m_outputs.insert(m_output_id - 1, output);
    }
    m_selected_coins = inputs;
    m_auto_selected_outpoints.clear();
    m_auto_coin_selection->setChecked(false);
    m_estimated_fee = transaction.fee;
    m_fee_toggle->setChecked(false);
    onFeeToggled();
    m_fee_value_input->setText(QString::number(m_estimated_fee));
    onUpdateFeeValidation();
    m_warning_label->setText(warning);
    m_warning_label->setVisible(!warning.isEmpty());
    view();
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

Send::Send(const Services &services, SendSigning *signing)
    : m_services(services),
      m_signing(signing) {
    qDebug() << "Send::Send()";
    this->init();
    this->doConnect();
    this->onAddOutput();
    this->view();
    this->onFeeToggled();
    initializeFees();
}

void Send::init() {
    qDebug() << "Send::init()";
    m_outputs_column = (new qontrol::Column);
    m_inputs_column = (new qontrol::Column);

    m_add_output_btn = catalog::btnAddPayment();
    m_clear_outputs_btn = catalog::btnClear();
    m_signing->setParent(this);
    m_last_balance = m_services.blockchain->balance();

    // Fee toggle: OFF = sats (absolute), ON = sats/vb (rate)
    m_fee_toggle = new Toggle;
    m_fee_toggle->setChecked(true); // Default to sats/vb mode

    m_fee_field = new catalog::ValidatedInput(InputRole::Mono);
    m_fee_value_input = m_fee_field->input();
    m_fee_value_input->setObjectName(QStringLiteral("fee-value"));
    m_fee_field->setWidth(Size::XS);
    m_fee_field->setFixedWidth(resolve(Size::XS));
    m_fee_value_input->setText("1");
    m_fee_mode_picker = new catalog::ComboBox;
    m_fee_mode_picker->setObjectName(QStringLiteral("fee-mode"));
    m_fee_mode_picker->addItems({TR("fee-manual"), TR("fee-default"), TR("fee-smart")});
    m_fee_tier_picker = new catalog::ComboBox;
    m_fee_tier_picker->setObjectName(QStringLiteral("fee-tier"));
    m_fee_tier_picker->addItems({TR("fee-low"), TR("fee-medium"), TR("fee-fast")});
    m_fee_tier_picker->setVisible(false);

    // The design's field label: an eyebrow over the toggle, the value and its unit.
    m_fee_rate_label = new Label(TR("send-fee-rate-label"), LabelRole::Eyebrow);
    m_fee_rate_label->setTone(catalog::LabelTone::Muted);

    m_fee_label = new Label(TR("send-fee-rate-unit"));
    m_fee_label->setFixedWidth(
        m_fee_label->fontMetrics().horizontalAdvance(TR("send-fee-rate-unit")) + 5);

    auto *feeValueRow = (new qontrol::Row)
                            ->spacing(resolve(Spacing::S))
                            ->push(m_fee_toggle)
                            ->push(m_fee_field)
                            ->push(m_fee_label);
    m_fee_rate_group = (new qontrol::Column)
                           ->spacing(resolve(Spacing::XS))
                           ->push(m_fee_rate_label)
                           ->push(feeValueRow);

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

    m_clear_inputs_btn = catalog::btnClear();

    m_auto_amount = new Label(QString(), LabelRole::Mono);
    m_auto_amount->setSize(catalog::LabelSize::Lg);
    m_auto_count = new Label(QString(), LabelRole::Caption);
    auto *autoRow = (new qontrol::Row)
                        ->spacing(resolve(Spacing::XS))
                        ->push(m_auto_amount)
                        ->push(new Label("BTC", LabelRole::Caption), 0, Qt::AlignBottom)
                        ->push(m_auto_count, 0, Qt::AlignBottom)
                        ->pushSpacer();
    m_auto_summary = new catalog::Card(catalog::Card::Role::Inset);
    m_auto_summary->setPad(catalog::Card::Pad::M);
    m_auto_summary->setContent(autoRow);

    m_inputs_title_label = new Label(TR("send-inputs"), LabelRole::Eyebrow);
    // Pin the title to the button's height so the row does not change height when
    // the (compact) Clear button shows/hides. defaultBtn has no fixed height, so
    // the effective button height is BUTTON_MIN_HEIGHT.
    m_inputs_title_label->setMinimumHeight(metric::BUTTON_MIN_HEIGHT);
}

void Send::doConnect() {
    connect(Host::get()->events(), &HostEvents::selectedFeesChanged, this,
            &Send::onSelectedFeesChanged, qontrol::UNIQUE);
    connect(m_fee_mode_picker, qOverload<int>(&QComboBox::activated), this,
            &Send::onFeeModeActivated, qontrol::UNIQUE);
    connect(m_fee_tier_picker, qOverload<int>(&QComboBox::activated), this,
            &Send::onFeeTierActivated, qontrol::UNIQUE);
    connect(m_fee_value_input, &QLineEdit::textEdited, this, &Send::onFeeEdited, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::selectedPriceChanged, this,
            &Send::onSelectedPriceChanged, qontrol::UNIQUE);
    qDebug() << "Send::doConnect()";
    connect(m_add_output_btn, &QPushButton::clicked, this, &Send::onAddOutput, qontrol::UNIQUE);
    connect(m_clear_outputs_btn, &QPushButton::clicked, this, &Send::onClearOutputs,
            qontrol::UNIQUE);
    connect(m_clear_inputs_btn, &QPushButton::clicked, this, &Send::onClearInputs, qontrol::UNIQUE);
    connect(m_auto_coin_selection, &Checkbox::clicked, this, &Send::onAutoSelectionToggled,
            qontrol::UNIQUE);
    connect(m_services.blockchain, &backend::IBlockchain::coinsChanged, this, &Send::onCoinsChanged,
            qontrol::UNIQUE);
    connect(m_services.transaction, &backend::ITransaction::coinsSelected, this,
            &Send::onCoinsSelected, qontrol::UNIQUE);
    connect(m_services.transaction, &backend::IService::error, this, &Send::onSelectionError,
            qontrol::UNIQUE);
    connect(m_signing, &SendSigning::lockedChanged, this, &Send::onSigningLocked, qontrol::UNIQUE);
    connect(m_signing, &SendSigning::estimateChanged, this, &Send::onProcess, qontrol::UNIQUE);
    connect(m_signing, &SendSigning::sent, this, &Send::onSigningSent, qontrol::UNIQUE);
    connect(m_fee_toggle, &QCheckBox::clicked, this, &Send::onFeeOverrideToggled, qontrol::UNIQUE);
    connect(m_fee_value_input, &QLineEdit::textChanged, this, &Send::onProcess, qontrol::UNIQUE);
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
    outputsScroll->setViewportHeight(outputsHeight);

    auto *addRow = (new qontrol::Row)->pushSpacer()->push(m_add_output_btn);

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
                    ->push(new catalog::SectionHeader(TR("send-recipients")))
                    ->push(topRule)
                    ->push(outputsScroll)
                    ->push(bottomRule)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(addRow);

    return col;
}

void Send::updateTransactionFlow() {
    if (m_flow_card == nullptr) {
        return;
    }

    QList<backend::Coin> selectedCoins;
    if (m_preview.has_value()) {
        for (qsizetype i = 0; i < m_preview->transaction.inputs.size(); ++i) {
            selectedCoins.append({.outpoint = m_preview->transaction.inputs.at(i),
                                  .amount = m_preview->inputValues.at(i)});
        }
    } else if (m_auto_coin_selection->isChecked()) {
        for (const auto &coin : availableCoins()) {
            if (m_auto_selected_outpoints.contains(coin.outpoint)) {
                selectedCoins.append(coin);
            }
        }
    } else {
        selectedCoins = m_selected_coins;
    }

    auto amounts = m_preview.has_value() ? m_preview->amounts : editableAmounts();
    auto selectedTotal = amounts.inputTotal;
    QList<catalog::TxFlow::InputItem> inputs;
    for (const auto &coin : selectedCoins) {
        catalog::TxFlow::InputItem item;
        item.weight = static_cast<double>(coin.amount) / SATS;
        item.title = TR("txflow-input-n").arg(inputs.size() + 1);
        item.fiat = fiatAmount(coin.amount);
        item.addr = coin.outpoint;
        inputs.append(item);
    }

    QList<catalog::TxFlow::OutputItem> outputs;
    auto sendingTotal = amounts.sendingTotal;
    qsizetype recipientIndex = 0;
    for (auto *output : m_outputs) {
        auto amount = output->amount();
        uint64_t value = m_preview.has_value()
                             ? m_preview->transaction.outputs.at(recipientIndex).amount
                             : amount.value_or(0);
        ++recipientIndex;
        if (value == 0) {
            continue;
        }
        catalog::TxFlow::OutputItem item;
        item.kind = catalog::TxFlow::OutputKind::Recipient;
        item.weight = static_cast<double>(value) / SATS;
        item.title = output->label().isEmpty() ? TR("txflow-output-n").arg(outputs.size() + 1)
                                               : output->label();
        item.label = output->label();
        item.fiat = fiatAmount(value);
        item.addr = output->address();
        outputs.append(item);
    }

    auto change = amounts.change;
    auto total = amounts.transactionTotal;

    if (change > 0) {
        catalog::TxFlow::OutputItem item;
        item.kind = catalog::TxFlow::OutputKind::Change;
        item.weight = static_cast<double>(change) / SATS;
        item.title = TR("txflow-change-title");
        item.fiat = fiatAmount(change);
        item.addr = TR("txflow-change-addr");
        outputs.append(item);
    }
    if (m_estimated_fee > 0 && selectedTotal > 0) {
        catalog::TxFlow::OutputItem item;
        item.kind = catalog::TxFlow::OutputKind::Fee;
        item.weight = static_cast<double>(m_estimated_fee) / SATS;
        item.title = TR("txflow-fee-title");
        item.fiat = fiatAmount(m_estimated_fee);
        item.addr = TR("txflow-fee-addr");
        outputs.append(item);
    }

    m_flow_card->setInputs(inputs);
    m_flow_card->setOutputs(outputs);

    // The fee, and what follows from it, once the signing step can tell it (a Multikey
    // account needs its spend path).
    bool estimated = m_estimated_fee > 0 && m_signing->estimatesFee();
    m_sending_btc->setText(sendingTotal > 0 ? toBitcoin(sendingTotal) : QString("--"));
    m_sending_fiat->setText(sendingTotal > 0 ? fiatAmount(sendingTotal) : QString());
    m_fee_btc->setText(estimated ? toBitcoin(m_estimated_fee) : QString("--"));
    m_fee_fiat->setText(estimated ? fiatAmount(m_estimated_fee) : QString());
    m_total_btc->setText(total > 0 && estimated ? toBitcoin(total) : QString("--"));
    m_total_fiat->setText(total > 0 && estimated ? fiatAmount(total) : QString());
    m_change_btc->setText(toBitcoin(change));
    m_change_fiat->setText(fiatAmount(change));
    bool hasPrice = Host::get()->selectedPrice().rate.has_value();
    m_sending_fiat->setVisible(hasPrice);
    m_fee_fiat->setVisible(hasPrice);
    m_total_fiat->setVisible(hasPrice);
    // Design hides the change row when there is no change.
    bool hasChange = change > 0 && estimated;
    m_change_label->setVisible(hasChange);
    m_change_btc->setVisible(hasChange);
    m_change_fiat->setVisible(hasChange && hasPrice);
}

auto Send::transactionView() -> QWidget * {
    m_flow_card = new catalog::TxFlowCard;

    // Transaction summary as a grid so the fiat and BTC values line up in two
    // right-aligned columns. The change row is always present and toggled by
    // updateTransactionFlow() so live updates never restructure the layout.
    auto *summary = new qontrol::Grid;
    summary->spacing(resolve(Spacing::M), resolve(Spacing::XS));
    // Full width: label on the left, the two value columns pushed to the right by
    // a stretch in the middle. Columns 2 (fiat, muted) and 3 (BTC) auto-align across
    // rows.
    summary->columnStretch(1, 1);
    int sr = 0;
    summary->push(new catalog::Separator, sr++, 0, 1, 4);

    summary->push(new Label(TR("send-summary-sending"), LabelRole::InfoLabel), sr, 0);
    m_sending_btc = new Label("--", LabelRole::Mono);
    m_sending_btc->setAlignment(Qt::AlignRight);
    m_sending_fiat = new Label(QString(), LabelRole::Mono);
    m_sending_fiat->setAlignment(Qt::AlignRight);
    m_sending_fiat->setTone(catalog::LabelTone::Muted);
    summary->push(m_sending_fiat, sr, 2);
    summary->push(m_sending_btc, sr++, 3);

    summary->push(new Label(TR("send-summary-fee"), LabelRole::InfoLabel), sr, 0);
    m_fee_btc = new Label("--", LabelRole::Mono);
    m_fee_btc->setAlignment(Qt::AlignRight);
    m_fee_fiat = new Label(QString(), LabelRole::Mono);
    m_fee_fiat->setAlignment(Qt::AlignRight);
    m_fee_fiat->setTone(catalog::LabelTone::Muted);
    summary->push(m_fee_fiat, sr, 2);
    summary->push(m_fee_btc, sr++, 3);

    m_change_label = new Label(TR("send-summary-change"), LabelRole::InfoLabel);
    summary->push(m_change_label, sr, 0);
    m_change_btc = new Label("--", LabelRole::Mono);
    m_change_btc->setAlignment(Qt::AlignRight);
    m_change_fiat = new Label(QString(), LabelRole::Mono);
    m_change_fiat->setAlignment(Qt::AlignRight);
    m_change_fiat->setTone(catalog::LabelTone::Muted);
    summary->push(m_change_fiat, sr, 2);
    summary->push(m_change_btc, sr++, 3);

    summary->push(new catalog::Separator, sr++, 0, 1, 4);

    summary->push(new Label(TR("send-summary-total"), LabelRole::Heading), sr, 0);
    m_total_btc = new Label("--", LabelRole::Mono);
    m_total_btc->setAlignment(Qt::AlignRight);
    m_total_btc->setScale(1.2);
    m_total_fiat = new Label(QString(), LabelRole::Mono);
    m_total_fiat->setAlignment(Qt::AlignRight);
    m_total_fiat->setTone(catalog::LabelTone::Muted);
    m_total_fiat->setScale(1.2);
    summary->push(m_total_fiat, sr, 2);
    summary->push(m_total_btc, sr, 3);

    auto *warningRow = (new qontrol::Row)->push(m_warning_label)->pushSpacer();
    // The design's fee row: mode, tier, then the fee rate under its label, the step's
    // buttons at the end, all on the bottom line.
    auto *feeInputs = (new qontrol::Row)
                          ->spacing(resolve(Spacing::S))
                          ->push(m_fee_mode_picker, 0, Qt::AlignBottom)
                          ->push(m_fee_tier_picker, 0, Qt::AlignBottom)
                          ->push(m_fee_rate_group, 0, Qt::AlignBottom);
    auto *buttons = (new qontrol::Row)
                        ->spacing(resolve(Spacing::S))
                        ->pushSpacer()
                        ->push(m_clear_outputs_btn, 0, Qt::AlignBottom)
                        ->push(m_signing->actions(), 0, Qt::AlignBottom);
    auto *actionRow = new ActionRow(feeInputs, buttons);

    // The Coins selection heading's style and height, so the two line up.
    auto *title = new Label(TR("send-transaction-title"), LabelRole::Eyebrow);
    title->setMinimumHeight(metric::BUTTON_MIN_HEIGHT);
    auto *col = new qontrol::Column;
    col->push(title)->pushSpacer(resolve(Spacing::S));
    if (!m_stacked) {
        // Side by side: the diagram on top, the summary and buttons at the bottom.
        col->push(m_flow_card)->pushStretch(1);
    }
    col->push(summary)
        ->pushSpacer(resolve(Spacing::M))
        ->push(actionRow)
        ->pushSpacer(resolve(Spacing::M))
        ->push(m_signing)
        ->push(warningRow);
    if (m_stacked) {
        // One column: the summary and buttons first, the diagram last.
        col->pushSpacer(resolve(Spacing::M))->push(m_flow_card);
    }

    applyLocked();
    updateTransactionFlow(); // populate the flow + summary with the current state
    return col;
}

void Send::applyLocked() {
    // Held by the signing step, the fee is settled: the fee inputs go, the summary keeps
    // the fee, and the step's own Cancel lets go of the transaction where Clear would
    // empty the payments.
    m_fee_rate_group->setVisible(!m_locked);
    m_fee_mode_picker->setVisible(!m_locked);
    m_fee_tier_picker->setVisible(!m_locked && m_fee_mode == FeeMode::Smart);
    m_clear_outputs_btn->setVisible(!m_locked);
    for (auto *output : m_outputs) {
        output->widget()->setEnabled(!m_locked);
    }
    m_add_output_btn->setEnabled(!m_locked);
    m_coin_table->setEnabled(!m_locked);
    m_auto_coin_selection->setEnabled(!m_locked);
    m_clear_inputs_btn->setEnabled(!m_locked);
}

auto Send::inputsView() -> QWidget * {
    qDebug() << "Send::inputsView()";

    // Unparent persistent rows
    m_inputs_amounts_row->setParent(nullptr);
    m_auto_summary->setParent(nullptr);

    m_coin_table = new catalog::Table;
    m_coin_table->setHeaderVisible(true);

    // The table's rows sit inset by its scroll area's gutters: inset the amounts row
    // the same, so the total selected value stays aligned with the value column.
    const int c_amounts_pad = resolve(Padding::S) + catalog::ScrollArea::insetH();
    m_inputs_amounts_row->layout()->setContentsMargins(c_amounts_pad, 0, c_amounts_pad, 0);

    m_coin_table->addColumn({.title = TR("coins-type"), .width = resolve(Size::XXS)});
    m_coin_table->addColumn(
        {.title = TR("coins-outpoint"), .width = resolve(Size::M), .role = DisplayRole::Outpoint});
    m_coin_table->addColumn(
        {.title = TR("coins-label"), .grow = true, .placeholder = QStringLiteral("-")});
    m_coin_table->addColumn({.title = TR("coins-value"),
                             .width = resolve(Size::S),
                             .alignment = Qt::AlignRight,
                             .headerAlignment = Qt::AlignRight,
                             .role = DisplayRole::Amount});

    updateCoinRows();
    connect(m_coin_table, &catalog::Table::rowSelectionChanged, this, &Send::onCoinSelectionChanged,
            qontrol::UNIQUE);
    updateCoinCheckboxes();

    m_auto_coin_selection->setParent(nullptr);
    m_inputs_title_label->setParent(nullptr);
    m_clear_inputs_btn->setParent(nullptr);

    auto *titleRow = (new qontrol::Row)
                         ->spacing(resolve(Spacing::S))
                         ->push(m_inputs_title_label)
                         ->pushSpacer()
                         ->push(m_clear_inputs_btn)
                         ->push(m_auto_coin_selection);

    // Update total and minimum displays
    onUpdateInputsTotal();

    applyInputsMode();

    auto *col = new qontrol::Column;
    col->push(titleRow)->pushSpacer(resolve(Spacing::S))->push(m_auto_summary)->push(m_coin_table);
    col->pushSpacer(resolve(Spacing::XS))->push(m_inputs_amounts_row);
    if (auto *coinsExtra = m_signing->coinsWidget(); coinsExtra != nullptr) {
        col->pushSpacer(resolve(Spacing::M))->push(coinsExtra);
    }
    // No trailing stretch here: in two-column mode it would inflate and push the
    // separator far below the table. The outputs frame carries the trailing stretch.

    return col;
}

void Send::updateCoinRows() {
    auto coins = m_restored ? m_selected_coins : availableCoins();
    constexpr int c_max_visible_coins = 6;
    auto visibleRows = std::min(static_cast<int>(coins.size()), c_max_visible_coins);
    m_coin_table->setBodyHeight(visibleRows * metric::TABLE_ROW_HEIGHT);
    m_coin_table->clearRows();
    for (const auto &coin : coins) {
        m_coin_table->addRow(coin.outpoint, {coin.origin, shortenOutpoint(coin.outpoint),
                                             coin.label, toBitcoin(coin.amount)});
    }
}

auto Send::availableCoins() const -> QList<backend::Coin> {
    QList<backend::Coin> coins;
    for (const auto &coin : m_services.blockchain->coins()) {
        if (!coin.spent) {
            coins.append(coin);
        }
    }
    return coins;
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
    // Only the coin table and the totals change: rebuilding the screen would make it
    // blink.
    updateCoinCheckboxes();
    onUpdateInputsTotal();
    onProcess();
}

void Send::onCoinSelectionChanged(const QString &row_id, bool selected) {
    if (selected) {
        for (const auto &coin : availableCoins()) {
            if (coin.outpoint == row_id) {
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
            if (m_selected_coins[i].outpoint == row_id) {
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

    if (checkbox->isChecked()) {
        // Add to m_selected_coins if not already present
        for (const auto &coin : availableCoins()) {
            if (coin.outpoint == outpoint) {
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
            if (m_selected_coins[i].outpoint == outpoint) {
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
    // Only the inputs section changes: rebuilding the screen would make it blink.
    applyInputsMode();
    updateCoinCheckboxes();
    onUpdateInputsTotal();
    onProcess();
}

void Send::onCoinsChanged() {
    if (m_locked) {
        return;
    }
    // Published with the coins, so it counts them already.
    auto balance = m_services.blockchain->balance();
    qDebug() << "Send::onCoinsChanged() confirmed:" << balance.confirmedCount
             << "unconfirmed:" << balance.unconfirmedCount;
    // Only rebuild if coins actually changed
    bool changed = (balance.confirmedCount != m_last_balance.confirmedCount ||
                    balance.unconfirmedCount != m_last_balance.unconfirmedCount);

    if (!changed) {
        return;
    }

    m_last_balance = balance;

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
    for (const auto &coin : availableCoins()) {
        if (m_auto_selected_outpoints.contains(coin.outpoint)) {
            m_selected_coins.append(coin);
        }
    }
}

auto Send::editableAmounts() -> money::Amounts {
    QList<quint64> inputs;
    if (m_auto_coin_selection->isChecked()) {
        for (const auto &coin : availableCoins()) {
            if (m_auto_selected_outpoints.contains(coin.outpoint)) {
                inputs.append(coin.amount);
            }
        }
    } else {
        for (const auto &coin : m_selected_coins) {
            inputs.append(coin.amount);
        }
    }

    QList<quint64> recipients;
    for (auto *out : m_outputs) {
        if (!out->isMax()) {
            auto amt = out->amount();
            if (amt.has_value()) {
                recipients.append(amt.value());
            }
        }
    }

    return money::amounts(inputs, recipients, m_estimated_fee);
}

void Send::onUpdateInputsTotal() {
    auto amounts = editableAmounts();
    auto totalSelected = m_preview.has_value() ? m_preview->amounts.inputTotal : amounts.inputTotal;
    auto remaining = m_preview.has_value() ? 0 : amounts.remaining;
    m_inputs_min_value->setText(QString::number(static_cast<double>(remaining) / SATS, 'f', 8) +
                                " BTC");
    m_inputs_total_value->setText(
        QString::number(static_cast<double>(totalSelected) / SATS, 'f', 8) + " BTC");
    m_auto_amount->setText(QString::number(static_cast<double>(totalSelected) / SATS, 'f', 8));
    m_auto_count->setText(
        QString("(%1)").arg(coinsCount(static_cast<uint64_t>(m_auto_selected_outpoints.size()))));
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
                selectedIds.append(coin.outpoint);
            }
        }
        m_coin_table->setSelectedIds(selectedIds);
        // Keep the table enabled in auto mode so it stays scrollable; rows are
        // already made non-selectable via setSelectionEnabled(!autoMode).
    }
    updateInputsTitle();
}

void Send::applyInputsMode() {
    qDebug() << "Send::applyInputsMode()";
    bool autoMode = m_auto_coin_selection->isChecked();
    // In auto mode the coins are picked for the user: the table is still shown
    // (and scrollable), but rows are not selectable.
    m_coin_table->setSelectionEnabled(!autoMode);
    m_clear_inputs_btn->setVisible(!autoMode); // Clear sits in the coins header, manual only
    // Stacked, the auto selection folds to its total; otherwise the coin table shows.
    bool folded = autoMode && m_stacked;
    m_auto_summary->setVisible(folded);
    m_coin_table->setVisible(!folded);
    m_inputs_amounts_row->setVisible(!folded);
}

void Send::updateInputsTitle() {
    qDebug() << "Send::updateInputsTitle()";
    // The automatic selection is not the user's: only a manual one is counted.
    int count = m_auto_coin_selection->isChecked() ? 0 : static_cast<int>(m_selected_coins.size());
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

void OutputW::setAddress(const QString &address) {
    m_address_input->setText(address);
}

auto OutputW::amount() -> std::optional<uint64_t> {
    if (m_frozen_amount.has_value()) {
        return m_frozen_amount;
    }
    if (m_parsed_text != m_amount_input->text()) {
        updateAmountValidation();
    }
    return m_parsed_amount;
}

void OutputW::syncFiatFromBtc() {
    auto price = Host::get()->selectedPrice();
    m_fiat_group->setVisible(price.rate.has_value());
    m_fiat_code->setText(price.currency);
    auto sats = amount();
    if (!sats.has_value() || !price.rate.has_value()) {
        m_amount_fiat_input->clear();
        return;
    }
    double btc = static_cast<double>(sats.value()) / SATS;
    m_amount_fiat_input->setText(QString::number(btc * price.rate.value(), 'f', 2));
}

void OutputW::syncBtcFromFiat() {
    auto price = Host::get()->selectedPrice();
    if (!price.rate.has_value() || isMax() || m_frozen_amount.has_value()) {
        return;
    }
    bool ok = false;
    auto fiat = m_amount_fiat_input->text().toDouble(&ok);
    auto btcText = ok && fiat > 0 ? QString::number(fiat / price.rate.value(), 'f', 8) : QString();
    // Update BTC once; onFiatEdited runs the transaction processing after conversion.
    QSignalBlocker blocker(m_amount_input);
    m_amount_input->setText(localizedMoneyText(*m_amount_input, btcText));
}

auto OutputW::amountInput() -> QWidget * {
    return m_amount_input;
}

auto OutputW::fiatInput() -> QWidget * {
    return m_amount_fiat_input;
}

auto OutputW::label() -> QString {
    return m_label_input->text();
}

void OutputW::updateAddressValidation() {
    QString addr = m_address_input->text();
    if (addr.isEmpty()) {
        setValidationIndicator(m_address_field, addr, false);
    } else {
        setValidationIndicator(m_address_field, addr, money::isAddress(addr));
    }
}

void OutputW::updateAmountValidation() {
    QString text = m_amount_input->text();
    m_parsed_text = text;
    auto normalized = normalizedMoneyText(*m_amount_input);
    m_parsed_amount = normalized.has_value() ? money::parseBtc(*normalized) : std::nullopt;
    setValidationIndicator(m_amount_field, text, m_parsed_amount.has_value());
}

auto Send::txTemplate() -> std::optional<backend::TransactionTemplate> {
    qDebug() << "Send::txTemplate()";
    if (!m_parsed_fee.has_value()) {
        return std::nullopt;
    }
    backend::TransactionTemplate transaction{.feeRate = m_parsed_fee->rate,
                                             .fee = m_parsed_fee->absolute};

    // Outputs
    for (auto *out : m_outputs) {
        backend::Output output{.address = out->address(), .label = out->label()};
        if (out->isMax()) {
            output.maximum = true; // Amount is ignored when maximum is set
        } else {
            auto amount = out->amount();
            if (!amount.has_value()) {
                return std::nullopt;
            }
            output.amount = amount.value();
        }
        transaction.outputs.append(output);
    }

    // The coins picked by hand; none leaves them to an explicit selection.
    if (!m_auto_coin_selection->isChecked()) {
        for (const auto &coin : m_selected_coins) {
            transaction.inputs.append(coin.outpoint);
        }
    }

    return transaction;
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
    auto normalized = normalizedMoneyText(*m_fee_value_input);
    m_parsed_fee = normalized.has_value() ? money::parseFee(*normalized, m_fee_toggle->isChecked())
                                          : std::nullopt;
    setValidationIndicator(m_fee_field, text, m_parsed_fee.has_value());
}

void Send::onProcess() {
    qDebug() << "Send::onProcess()";
    // The signing step holds the transaction: nothing it is made of can change.
    if (m_locked) {
        return;
    }
    m_preview.reset();
    m_selection_request = 0;
    m_estimated_fee = 0;
    for (auto *out : m_outputs) {
        if (sender() != out->fiatInput()) {
            out->syncFiatFromBtc();
        }
    }
    onUpdateOutputValidations();
    onUpdateFeeValidation();
    onUpdateInputsTotal();

    auto txTemp = txTemplate();
    if (!txTemp.has_value()) {
        m_signing->setTransaction(std::nullopt, {});
        // Clear auto selection when outputs are invalid
        if (m_auto_coin_selection->isChecked() && !m_auto_selected_outpoints.isEmpty()) {
            m_auto_selected_outpoints.clear();
            updateCoinCheckboxes();
            onUpdateInputsTotal();
        }
        updateTransactionFlow();
        return;
    }

    if (!txTemp->inputs.isEmpty()) {
        applyCraft(m_services.transaction->craft(*txTemp));
        return;
    }
    // Nothing to sign until the selection answers; the coins it spent stay, so the step
    // keeps what it chose for them.
    m_signing->setTransaction(std::nullopt, spentOutpoints());
    // Registered before the request: the answer may come during the call.
    m_selection_request = nextRequestId();
    m_services.transaction->selectCoins(m_selection_request, *txTemp);
}

void Send::applyCraft(const backend::CraftResult &result) {
    if (!result.completion.complete) {
        clearTransaction(result.completion.error.isEmpty() ? TR("send-transaction-invalid")
                                                           : operationFailedSummary());
        return;
    }
    const auto &crafted = result.transaction;
    m_warning_label->setVisible(false);
    QHash<QString, quint64> values;
    for (const auto &coin : availableCoins()) {
        values.insert(coin.outpoint, coin.amount);
    }
    Preview preview{.transaction = crafted};
    for (const auto &input : crafted.inputs) {
        preview.inputValues.append(values.value(input));
    }
    QList<quint64> recipients;
    for (const auto &output : crafted.outputs) {
        recipients.append(output.amount);
    }
    preview.amounts = money::amounts(preview.inputValues, recipients, crafted.fee);
    m_preview = preview;

    // Fill in estimated max amount for MAX outputs
    qsizetype recipientIndex = 0;
    for (auto *out : m_outputs) {
        if (out->isMax()) {
            out->setMaxAmount(crafted.outputs.at(recipientIndex).amount);
        }
        ++recipientIndex;
    }

    // Store auto-selected outpoints for display when auto mode is active
    if (m_auto_coin_selection->isChecked()) {
        m_auto_selected_outpoints = crafted.inputs;
        updateCoinCheckboxes();
        onUpdateInputsTotal();
    }
    m_signing->setTransaction(crafted, spentOutpoints());
    m_estimated_fee = crafted.fee;

    onUpdateInputsTotal();
    // Reflect the updated inputs/outputs/fee in the graphical flow as the user types.
    updateTransactionFlow();
}

void Send::clearTransaction(const QString &warning) {
    m_signing->setTransaction(std::nullopt, {});
    m_estimated_fee = 0;
    m_warning_label->setText(warning);
    m_warning_label->setVisible(!warning.isEmpty());
    onUpdateInputsTotal();
    updateTransactionFlow();
}

void Send::onFiatEdited() {
    if (m_locked || sender() == nullptr) {
        return;
    }
    auto *output = m_outputs.value(sender()->property("outputId").toInt(), nullptr);
    if (output == nullptr || output->isMax()) {
        return;
    }
    output->syncBtcFromFiat();
    onProcess();
}

void Send::onSelectedPriceChanged() {
    for (auto *output : m_outputs) {
        output->syncFiatFromBtc();
    }
    updateTransactionFlow();
}

auto Send::spentOutpoints() const -> QStringList {
    if (m_auto_coin_selection->isChecked()) {
        return m_auto_selected_outpoints;
    }
    QStringList outpoints;
    for (const auto &coin : m_selected_coins) {
        outpoints.append(coin.outpoint);
    }
    return outpoints;
}

void Send::onCoinsSelected(backend::RequestId request_id, const backend::CraftResult &result) {
    // A stale answer, or one that came after the signing step took the transaction.
    if (request_id == 0 || request_id != m_selection_request || m_locked) {
        return;
    }
    m_selection_request = 0;
    applyCraft(result);
}

void Send::onSelectionError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0 || request_id != m_selection_request || m_locked) {
        return;
    }
    m_selection_request = 0;
    qWarning() << "Send: coin selection failed:" << message;
    clearTransaction(operationFailedSummary());
}

void Send::onSigningLocked(bool locked) {
    auto wasRestored = m_restored;
    m_locked = locked;
    if (!locked) {
        m_restored = false;
        for (auto *output : m_outputs) {
            output->setFrozenAmount(std::nullopt);
            if (wasRestored) {
                output->enableMax(true);
                output->setDeletable(m_outputs.size() > 1);
            }
        }
        if (wasRestored) {
            updateCoinRows();
            updateCoinCheckboxes();
            applyInputsMode();
        }
    }
    applyLocked();
    if (!locked) {
        onSelectedFeesChanged();
        onProcess();
    }
}

void Send::onSigningSent() {
    onClearOutputs();
    onClearInputs();
    initializeFees();
}

void Send::initializeFees() {
    auto preferences = Host::get()->selectedFees().preferences;
    m_fee_mode = preferences.mode;
    m_fee_tier = FeeTier::Medium;
    m_fee_mode_picker->setCurrentIndex(static_cast<int>(m_fee_mode));
    m_fee_tier_picker->setCurrentIndex(static_cast<int>(m_fee_tier));
    m_fee_tier_picker->setVisible(m_fee_mode == FeeMode::Smart);
    m_fee_toggle->setChecked(true);
    onFeeToggled();
    m_fee_value_input->setText(QStringLiteral("1"));
    onSelectedFeesChanged();
}

void Send::onSelectedFeesChanged() {
    if (m_locked || m_restored || m_fee_mode == FeeMode::Manual) {
        return;
    }
    auto rate = Host::get()->selectedFees().rate(m_fee_mode, m_fee_tier);
    if (!rate.has_value()) {
        m_fee_mode = FeeMode::Manual;
        m_fee_mode_picker->setCurrentIndex(static_cast<int>(m_fee_mode));
    }
    m_fee_tier_picker->setVisible(m_fee_mode == FeeMode::Smart);
    auto text = QString::number(rate.value_or(1.0), 'g', 15);
    if (rate.has_value()) {
        auto normalized = money::normalizeFeeRate(rate.value());
        if (normalized.has_value()) {
            text = QString::number(*normalized, 'f', 3);
        }
    }
    m_fee_value_input->setText(localizedMoneyText(*m_fee_value_input, text));
}

void Send::onFeeEdited() {
    if (m_locked || m_restored) {
        return;
    }
    m_fee_mode = FeeMode::Manual;
    m_fee_mode_picker->setCurrentIndex(static_cast<int>(m_fee_mode));
    m_fee_tier_picker->setVisible(false);
}

void Send::onFeeOverrideToggled() {
    onFeeEdited();
    onFeeToggled();
}

void Send::onFeeModeActivated(int index) {
    if (m_locked || m_restored) {
        return;
    }
    m_fee_mode = static_cast<FeeMode>(index);
    m_fee_tier_picker->setVisible(m_fee_mode == FeeMode::Smart);
    if (m_fee_mode != FeeMode::Manual) {
        m_fee_toggle->setChecked(true);
        onFeeToggled();
        onSelectedFeesChanged();
    }
}

void Send::onFeeTierActivated(int index) {
    if (m_locked || m_restored) {
        return;
    }
    m_fee_tier = static_cast<FeeTier>(index);
    onSelectedFeesChanged();
}

void Send::onSavedWarningChanged(const QString &message) {
    if (m_restored) {
        m_warning_label->setText(message);
        m_warning_label->setVisible(!message.isEmpty());
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

void Send::onScanAddressClicked() {
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (btn == nullptr) {
        return;
    }
    int id = btn->property("outputId").toInt();
    auto *scan = new modal::ScanAddress;
    bool accepted = scan->exec() == QDialog::Accepted;
    QString text = scan->text();
    delete scan;
    // The payment may be gone while the camera ran.
    auto *output = m_outputs.value(id);
    if (accepted && output != nullptr) {
        output->setAddress(text);
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

} // namespace account::view
