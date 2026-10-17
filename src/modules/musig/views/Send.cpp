#include "modules/musig/views/Send.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Display.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/MessageModal.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountController.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QDoubleValidator>
#include <QEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <Qontrol>
#include <QtGlobal>
#include <algorithm>
#include <common.h>
#include <cstddef>
#include <utility>

namespace musig::view {

using catalog::DisplayRole;
using catalog::Input;
using catalog::InputRole;
using catalog::Label;
using catalog::LabelRole;
using catalog::ValidationMark;

namespace {

const char *const PAYMENT_ID = "paymentId";
const char *const DEFAULT_FEE_RATE = "1";
constexpr int MAX_VISIBLE_COINS = 6;
constexpr double MAX_BTC = 21e6;
constexpr int BTC_DECIMALS = 8;
constexpr double MIN_FEE_RATE = 0.001;
constexpr double MAX_FEE_RATE = 1000000.0;
constexpr int FEE_RATE_DECIMALS = 3;

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

void setMark(ValidationMark *mark, const QString &text, bool valid) {
    if (text.isEmpty()) {
        mark->setState(ValidationMark::State::None);
    } else {
        mark->setState(valid ? ValidationMark::State::Valid : ValidationMark::State::Invalid);
    }
}

// The cosigner gave its share for the PSBT's current round.
auto shareIn(MusigRound round, const MusigCosignerStatus &cosigner) -> bool {
    switch (round) {
    case MusigRound::Nonces:
        return cosigner.nonces >= cosigner.expected;
    case MusigRound::PartialSigs:
        return cosigner.partial_sigs >= cosigner.expected;
    case MusigRound::Ready:
        return true;
    default:
        return false;
    }
}

} // namespace

// ===== PaymentW =====

PaymentW::PaymentW(Send *screen, int id) {
    m_address_input = new Input(InputRole::Mono);
    m_address_input->setPlaceholderText(TR("send-placeholder-address"));
    m_address_mark = new ValidationMark;

    m_amount_input = new Input(InputRole::Mono);
    m_amount_input->setWidth(Size::S);
    m_amount_input->setPlaceholderText(TR("send-placeholder-amount"));
    auto *validator = new QDoubleValidator(0, MAX_BTC, BTC_DECIMALS, m_amount_input);
    validator->setNotation(QDoubleValidator::StandardNotation);
    m_amount_input->setValidator(validator);
    m_amount_mark = new ValidationMark;

    m_remove_btn = catalog::btnRemovePayment();
    m_remove_btn->setProperty(PAYMENT_ID, id);

    QObject::connect(m_address_input, &QLineEdit::textChanged, screen, &Send::onProcess,
                     qontrol::UNIQUE);
    QObject::connect(m_amount_input, &QLineEdit::textChanged, screen, &Send::onProcess,
                     qontrol::UNIQUE);
    QObject::connect(m_remove_btn, &QPushButton::clicked, screen, &Send::onRemoveOutputClicked,
                     qontrol::UNIQUE);

    m_address_row = (new qontrol::Row)
                        ->spacing(resolve(Spacing::XS))
                        ->push(m_address_input, 1)
                        ->push(m_address_mark);
    m_amount_row = (new qontrol::Row)
                       ->spacing(resolve(Spacing::XS))
                       ->push(m_amount_input)
                       ->push(new Label("BTC", LabelRole::Subheading))
                       ->push(m_amount_mark)
                       ->pushSpacer();
    m_fields = (new catalog::FieldGroup(catalog::FieldGroup::Labels::Payment))
                   ->addField(TR("send-address"), m_address_row)
                   ->addField(TR("send-amount"), m_amount_row);

    auto *card = new catalog::Card;
    card->setContent(m_fields);
    card->setCorner(m_remove_btn);
    m_widget = card;
}

auto PaymentW::widget() -> QWidget * {
    return m_widget;
}

void PaymentW::setRemovable(bool removable) {
    m_remove_btn->setVisible(removable);
}

void PaymentW::setEditable(bool editable) {
    m_address_input->setEnabled(editable);
    m_amount_input->setEnabled(editable);
    m_remove_btn->setEnabled(editable);
}

auto PaymentW::address() const -> QString {
    return m_address_input->text().trimmed();
}

auto PaymentW::amount() const -> std::optional<uint64_t> {
    bool ok = false;
    double btc = m_amount_input->text().toDouble(&ok);
    if (!ok || btc <= 0.0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(qRound64(btc * SATS));
}

void PaymentW::updateValidation() {
    auto addr = address();
    setMark(m_address_mark, addr,
            !addr.isEmpty() && ::validate_address(rust::String(addr.toStdString())).empty());
    setMark(m_amount_mark, m_amount_input->text(), amount().has_value());
}

void PaymentW::retranslate() {
    m_address_input->setPlaceholderText(TR("send-placeholder-address"));
    m_amount_input->setPlaceholderText(TR("send-placeholder-amount"));
    m_remove_btn->setToolTip(TR("btn-remove-payment"));
    m_fields->setFieldLabel(m_address_row, TR("send-address"));
    m_fields->setFieldLabel(m_amount_row, TR("send-amount"));
}

// ===== Send =====

Send::Send(MusigAccountController *ctrl) : m_controller(ctrl) {
    if (m_controller == nullptr) {
        return;
    }
    init();
    doConnect();
    view();
    onAddOutput();
    fillCoins();
}

void Send::init() {
    m_payments_column = new qontrol::Column;
    m_payments_column->spacing(resolve(Spacing::XS));
    m_add_payment_btn = catalog::btnAddPayment();

    m_fee_input = new Input(InputRole::Mono);
    m_fee_input->setWidth(Size::XS);
    auto *validator =
        new QDoubleValidator(MIN_FEE_RATE, MAX_FEE_RATE, FEE_RATE_DECIMALS, m_fee_input);
    validator->setNotation(QDoubleValidator::StandardNotation);
    m_fee_input->setValidator(validator);
    m_fee_input->setText(DEFAULT_FEE_RATE);
    m_fee_mark = new ValidationMark;
    m_fee_estimate_label = new Label;

    m_coin_table = new catalog::Table;
    m_coin_table->setSelectionEnabled(true);
    addCoinColumns();

    m_leaf_combo = new catalog::ComboBox;
    m_leaf_combo->setWidth(Size::XL);

    m_prepare_btn = catalog::btnPrepare();
    m_prepare_btn->setEnabled(false);
    m_prepare_reason_label = new Label(LabelRole::Caption);
    m_prepare_reason_label->setVisible(false);

    initSigningCard();
}

void Send::initSigningCard() {
    m_txid_caption = new Label(TR("history-txid"), LabelRole::InfoLabel);
    m_txid_label = new Label(LabelRole::Mono);
    m_txid_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_round_label = new Label(LabelRole::Subheading);
    m_cosigners_column = new qontrol::Column;
    m_cosigners_column->spacing(resolve(Spacing::XS));
    m_notice = new catalog::Notice;
    m_notice->setVisible(false);
    m_cancel_btn = catalog::btnCancelSigned();
    m_sign_btn = catalog::btnSign();
    m_broadcast_btn = catalog::btnBroadcast();
    m_broadcast_btn->setVisible(false);

    auto *txidRow = (new qontrol::Row)
                        ->spacing(resolve(Spacing::XS))
                        ->push(m_txid_caption)
                        ->push(m_txid_label)
                        ->pushSpacer();
    auto *buttons = (new qontrol::Row)
                        ->spacing(resolve(Spacing::S))
                        ->push(m_cancel_btn)
                        ->pushSpacer()
                        ->push(m_sign_btn)
                        ->push(m_broadcast_btn);
    auto *content = (new qontrol::Column)
                        ->spacing(resolve(Spacing::S))
                        ->push(txidRow)
                        ->push(m_round_label)
                        ->push(m_cosigners_column)
                        ->push(m_notice)
                        ->push(buttons);

    m_signing_card = new catalog::Card;
    m_signing_card->setContent(content);
    m_signing_card->setVisible(false);
}

void Send::doConnect() {
    connect(m_add_payment_btn, &QPushButton::clicked, this, &Send::onAddOutput, qontrol::UNIQUE);
    connect(m_fee_input, &QLineEdit::textChanged, this, &Send::onProcess, qontrol::UNIQUE);
    connect(m_coin_table, &catalog::Table::rowSelectionChanged, this, &Send::onCoinRowToggled,
            qontrol::UNIQUE);
    connect(m_leaf_combo, &QComboBox::currentIndexChanged, this, &Send::onSpendLeafChosen,
            qontrol::UNIQUE);
    connect(m_prepare_btn, &QPushButton::clicked, this, &Send::onPrepareClicked, qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &Send::onCancelClicked, qontrol::UNIQUE);
    connect(m_sign_btn, &QPushButton::clicked, this, &Send::onSignRoundClicked, qontrol::UNIQUE);
    connect(m_broadcast_btn, &QPushButton::clicked, this, &Send::onBroadcastClicked,
            qontrol::UNIQUE);

    connect(m_controller, &MusigAccountController::prepared, this, &Send::onPrepared,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::prepareFailed, this, &Send::onPrepareFailed,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::roundChanged, this, &Send::onRoundChanged,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::signingFailed, this, &Send::onSigningFailed,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::signingCancelled, this,
            &Send::onSigningCancelled, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::transactionReady, this,
            &Send::onTransactionReady, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::finalizeFailed, this, &Send::onFinalizeFailed,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::broadcastDone, this, &Send::onBroadcastDone,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::updateCoins, this, &Send::onCoinsUpdated,
            qontrol::UNIQUE);
}

void Send::view() {
    auto *addRow = (new qontrol::Row)->pushSpacer()->push(m_add_payment_btn);
    auto *feeRow = (new qontrol::Row)
                       ->spacing(resolve(Spacing::XS))
                       ->push(new Label(TR("send-fee-rate-label"), LabelRole::InfoLabel))
                       ->push(m_fee_input)
                       ->push(new Label(TR("send-fee-rate-unit")))
                       ->push(m_fee_mark)
                       ->pushSpacer(resolve(Spacing::M))
                       ->push(m_fee_estimate_label)
                       ->pushSpacer();
    auto *prepareRow = (new qontrol::Row)
                           ->spacing(resolve(Spacing::S))
                           ->push(m_prepare_reason_label)
                           ->pushSpacer()
                           ->push(m_prepare_btn);

    auto *col = (new qontrol::Column)
                    ->push(new catalog::SectionHeader(TR("send-recipients")))
                    ->push(m_payments_column)
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(addRow)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::Separator)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::SectionHeader(TR("send-inputs")))
                    ->push(m_coin_table)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(feeRow)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::SectionHeader(TR("musig-send-spend-path")))
                    ->push(m_leaf_combo)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(prepareRow)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(m_signing_card)
                    ->pushSpacer();

    setScreenContent(this, m_main_widget, dashboard(TR("send-action-send"), col));
}

void Send::changeEvent(QEvent *event) {
    // Without a controller the constructor built no widget.
    if (event->type() == QEvent::LanguageChange && m_controller != nullptr) {
        retranslateUi();
        view();
    }
    qontrol::Screen::changeEvent(event);
}

void Send::retranslateUi() {
    for (auto *payment : m_payments) {
        payment->retranslate();
    }
    m_add_payment_btn->setText(TR("btn-add-payment"));
    m_coin_table->clearColumns();
    addCoinColumns();
    auto keys = keyEntries();
    for (std::size_t i = 0; i < m_leaves.leaves.size(); ++i) {
        m_leaf_combo->setItemText(static_cast<int>(i), leafText(m_leaves.leaves[i], keys));
    }
    m_prepare_btn->setText(TR("musig-send-prepare"));
    m_txid_caption->setText(TR("history-txid"));
    m_cancel_btn->setText(TR("common-cancel"));
    m_broadcast_btn->setText(TR("common-broadcast"));
    m_notice->setTitle(failureTitle());
    // Fee estimate and the reason Prepare is off.
    onProcess();
    if (!m_signing_card->isHidden()) {
        onRoundChanged(m_controller->roundStatus());
    }
}

auto Send::failureTitle() const -> QString {
    switch (m_failure) {
    case Failure::SessionLost:
        return TR("musig-send-session-lost");
    case Failure::Finalize:
        return TR("musig-send-finalize-failed");
    case Failure::Sign:
        return {};
    }
    return {};
}

void Send::addCoinColumns() {
    m_coin_table->addColumn(
        {.title = TR("coins-outpoint"), .width = resolve(Size::M), .role = DisplayRole::Outpoint});
    m_coin_table->addColumn(
        {.title = TR("coins-label"), .grow = true, .placeholder = QStringLiteral("-")});
    m_coin_table->addColumn({.title = TR("coins-value"),
                             .width = resolve(Size::S),
                             .alignment = Qt::AlignRight,
                             .headerAlignment = Qt::AlignRight,
                             .role = DisplayRole::Amount});
}

auto Send::txTemplate() -> std::optional<TransactionTemplate> {
    bool ok = false;
    double feeRate = m_fee_input->text().toDouble(&ok);
    if (!ok || feeRate <= 0.0 || m_payments.empty()) {
        return std::nullopt;
    }
    TransactionTemplate tx;
    tx.fee_rate = feeRate;
    tx.fee = 0;
    for (auto *payment : m_payments) {
        auto amount = payment->amount();
        if (payment->address().isEmpty() || !amount.has_value()) {
            return std::nullopt;
        }
        Output output;
        output.address = rust::String(payment->address().toStdString());
        output.amount = amount.value();
        output.max = false;
        tx.outputs.push_back(output);
    }
    // No selection leaves input_outpoints empty: the account selects the coins.
    for (const auto &outpoint : m_coin_table->selectedIds()) {
        tx.input_outpoints.push_back(rust::String(outpoint.toStdString()));
    }
    return tx;
}

void Send::fillCoins() {
    auto selected = m_coin_table->selectedIds();
    QStringList kept;
    m_coin_table->clearRows();
    auto coins = m_controller->getCoins();
    for (const auto &coin : coins) {
        auto outpoint = toQString(coin.outpoint);
        m_coin_table->addRow(
            outpoint, {shortenOutpoint(outpoint), toQString(coin.label), toBitcoin(coin.value)});
        if (selected.contains(outpoint)) {
            kept.append(outpoint);
        }
    }
    auto visible = std::min(static_cast<int>(coins.size()), MAX_VISIBLE_COINS);
    m_coin_table->setBodyHeight(visible * metric::TABLE_ROW_HEIGHT);
    m_coin_table->setSelectedIds(kept);
    // The spend path of a running session never changes.
    if (m_signing_card->isHidden()) {
        onCoinsSelected(kept);
    }
}

auto Send::keyEntries() const -> QHash<QString, KeyEntry> {
    QHash<QString, KeyEntry> entries;
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return entries;
    }
    for (const auto &entry : keys->keys(m_controller->network())) {
        entries.insert(entry.fingerprint.toLower(), entry);
    }
    return entries;
}

auto Send::leafText(const MusigSpendLeaf &leaf, const QHash<QString, KeyEntry> &keys) -> QString {
    QStringList names;
    for (const auto &fingerprint : leaf.fingerprints) {
        auto fp = toQString(fingerprint);
        auto entry = keys.value(fp.toLower());
        names.append(entry.alias.isEmpty() ? fp : entry.alias);
    }
    QStringList parts;
    if (leaf.key_path) {
        parts.append(TR("musig-send-key-path"));
    }
    parts.append(names.join(QStringLiteral(" + ")));
    if (leaf.relative != 0) {
        parts.append(TR("musig-send-after-blocks").arg(leaf.relative));
    }
    if (leaf.absolute != 0) {
        parts.append(TR("musig-send-from-height").arg(leaf.absolute));
    }
    if (!leaf.spendable) {
        parts.append(TR("musig-send-locked"));
    }
    return parts.join(QStringLiteral(" "));
}

// The first spendable leaf every key of which signs here, else the first spendable one.
auto Send::defaultLeafIndex(const QHash<QString, KeyEntry> &keys) const -> int {
    int firstSpendable = -1;
    for (std::size_t i = 0; i < m_leaves.leaves.size(); ++i) {
        const auto &leaf = m_leaves.leaves[i];
        if (!leaf.spendable) {
            continue;
        }
        if (firstSpendable < 0) {
            firstSpendable = static_cast<int>(i);
        }
        bool allSign = true;
        for (const auto &fingerprint : leaf.fingerprints) {
            if (!keys.value(toQString(fingerprint).toLower()).can_sign) {
                allSign = false;
                break;
            }
        }
        if (allSign) {
            return static_cast<int>(i);
        }
    }
    return firstSpendable < 0 ? 0 : firstSpendable;
}

auto Send::chosenLeaf() const -> const MusigSpendLeaf * {
    if (m_leaf_index < 0 || std::cmp_greater_equal(m_leaf_index, m_leaves.leaves.size())) {
        return nullptr;
    }
    return &m_leaves.leaves[m_leaf_index];
}

void Send::updatePrepareState() {
    // A signing session or a transaction ready to broadcast holds the PSBT in flight.
    if (!m_signing_card->isHidden()) {
        m_prepare_btn->setEnabled(false);
        m_prepare_reason_label->setVisible(false);
        return;
    }
    const auto *leaf = chosenLeaf();
    QString reason;
    if (m_controller->getCoins().empty()) {
        reason = TR("musig-send-no-coins");
    } else if (!txTemplate().has_value()) {
        reason = TR("send-invalid-transaction-template");
    } else if (!m_simulation_error.isEmpty()) {
        reason = m_simulation_error;
    } else if (!m_leaves.is_ok) {
        reason = toQString(m_leaves.error);
    } else if (leaf == nullptr) {
        reason = TR("musig-send-no-path");
    } else if (!leaf->spendable) {
        reason = TR("musig-send-path-locked");
    }
    m_prepare_btn->setEnabled(reason.isEmpty());
    m_prepare_reason_label->setText(reason);
    m_prepare_reason_label->setVisible(!reason.isEmpty());
}

void Send::setEditable(bool editable) {
    for (auto *payment : m_payments) {
        payment->setEditable(editable);
    }
    m_add_payment_btn->setEnabled(editable);
    m_fee_input->setEnabled(editable);
    m_coin_table->setEnabled(editable);
    m_leaf_combo->setEnabled(editable);
    if (editable) {
        updatePrepareState();
    } else {
        m_prepare_btn->setEnabled(false);
        m_prepare_reason_label->setVisible(false);
    }
}

void Send::clearPayments() {
    for (auto *payment : m_payments) {
        m_payments_column->remove(payment->widget());
        delete payment;
    }
    m_payments.clear();
    onAddOutput();
}

void Send::updateRemovable() {
    for (auto *payment : m_payments) {
        payment->setRemovable(m_payments.size() > 1);
    }
}

// ===== Slots =====

void Send::onProcess() {
    for (auto *payment : m_payments) {
        payment->updateValidation();
    }
    bool ok = false;
    double feeRate = m_fee_input->text().toDouble(&ok);
    setMark(m_fee_mark, m_fee_input->text(), ok && feeRate > 0.0);

    m_simulation_error.clear();
    m_fee_estimate_label->setVisible(false);
    auto tx = txTemplate();
    if (!tx.has_value()) {
        updatePrepareState();
        return;
    }
    // A failed simulation goes to the reason line, under the no coin reason first.
    auto simulation = m_controller->simulateTx(tx.value());
    if (!simulation.error.empty()) {
        m_simulation_error = mapBackendErrorSummary(toQString(simulation.error));
    } else if (!simulation.is_valid) {
        m_simulation_error = TR("send-transaction-invalid");
    } else {
        m_fee_estimate_label->setText(TR("send-fee-estimate").arg(toBitcoin(simulation.fee)));
        m_fee_estimate_label->setVisible(true);
    }
    updatePrepareState();
}

void Send::onAddOutput() {
    auto *payment = new PaymentW(this, m_next_payment_id);
    m_payments.insert(m_next_payment_id, payment);
    m_next_payment_id++;
    m_payments_column->push(payment->widget());
    updateRemovable();
    onProcess();
}

void Send::onRemoveOutputClicked() {
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (btn == nullptr) {
        return;
    }
    auto *payment = m_payments.take(btn->property(PAYMENT_ID).toInt());
    if (payment == nullptr) {
        return;
    }
    // remove() deletes later: the button is still inside its clicked signal.
    m_payments_column->remove(payment->widget());
    delete payment;
    updateRemovable();
    onProcess();
}

void Send::onCoinsUpdated([[maybe_unused]] CoinState state) {
    fillCoins();
}

void Send::onCoinRowToggled([[maybe_unused]] const QString &row_id,
                            [[maybe_unused]] bool selected) {
    onCoinsSelected(m_coin_table->selectedIds());
}

void Send::onCoinsSelected(const QStringList &outpoints) {
    auto inputs = outpoints;
    if (inputs.isEmpty()) {
        for (const auto &coin : m_controller->getCoins()) {
            inputs.append(toQString(coin.outpoint));
        }
    }
    // Keep the path the user picked when the new list still has it.
    const auto *chosen = chosenLeaf();
    bool hadChoice = chosen != nullptr;
    QString previous;
    if (hadChoice) {
        previous = chosen->key_path ? QString() : toQString(chosen->leaf_hash);
    }

    m_leaves = m_controller->spendLeaves(inputs);
    auto keys = keyEntries();
    int index = defaultLeafIndex(keys);
    m_leaf_combo->clear();
    for (std::size_t i = 0; i < m_leaves.leaves.size(); ++i) {
        const auto &leaf = m_leaves.leaves[i];
        m_leaf_combo->addItem(leafText(leaf, keys));
        auto hash = leaf.key_path ? QString() : toQString(leaf.leaf_hash);
        if (hadChoice && hash == previous) {
            index = static_cast<int>(i);
        }
    }
    if (m_leaves.leaves.empty()) {
        index = -1;
    }
    m_leaf_combo->setCurrentIndex(index);
    onSpendLeafChosen(index);
    onProcess();
}

void Send::onSpendLeafChosen(int index) {
    m_leaf_index = index;
    updatePrepareState();
}

void Send::onPrepareClicked() {
    auto tx = txTemplate();
    const auto *leaf = chosenLeaf();
    if (!tx.has_value() || leaf == nullptr) {
        return;
    }
    m_controller->prepareTransaction(tx.value(), *leaf);
}

void Send::onPrepared([[maybe_unused]] const QString &psbt_v2_hex, const QString &txid) {
    m_txid_label->setText(txid);
    m_notice->setVisible(false);
    m_sign_btn->setVisible(true);
    m_sign_btn->setEnabled(true);
    m_broadcast_btn->setVisible(false);
    m_cancel_btn->setEnabled(true);
    setEditable(false);
    m_signing_card->setVisible(true);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void Send::onPrepareFailed(const QString &message) {
    AppController::execModal(new catalog::MessageModal(TR("send-prepare-failed"), message));
}

void Send::onRoundChanged(const MusigRoundStatus &status) {
    for (auto *row : m_cosigner_rows) {
        delete m_cosigners_column->take(row);
    }
    m_cosigner_rows.clear();
    // Once finalized only Broadcast is left, a single-key leaf included.
    bool finalized = !m_controller->finalizedTx().isEmpty();
    m_broadcast_btn->setVisible(finalized);
    if (!status.is_ok) {
        m_round_label->setText(toQString(status.error));
        m_sign_btn->setVisible(false);
        return;
    }
    switch (status.round) {
    case MusigRound::Nonces:
        m_round_label->setText(TR("musig-send-round-1"));
        m_sign_btn->setText(TR("musig-send-sign-round").arg(1));
        break;
    case MusigRound::PartialSigs:
        m_round_label->setText(TR("musig-send-round-2"));
        m_sign_btn->setText(TR("musig-send-sign-round").arg(2));
        break;
    case MusigRound::Ready:
        m_round_label->setText(TR("musig-send-ready"));
        break;
    case MusigRound::NotMusig:
        m_round_label->setText(TR("musig-send-single-key"));
        m_sign_btn->setText(TR("btn-sign"));
        break;
    default:
        break;
    }
    m_sign_btn->setVisible(!finalized && status.round != MusigRound::Ready);

    auto keys = keyEntries();
    for (const auto &cosigner : status.cosigners) {
        auto fp = toQString(cosigner.fingerprint);
        auto entry = keys.value(fp.toLower());
        auto *row =
            (new qontrol::Row)
                ->spacing(resolve(Spacing::M))
                ->push(new Label(entry.alias.isEmpty() ? fp : entry.alias))
                ->push(
                    new Label(TR("musig-send-nonces").arg(cosigner.nonces).arg(cosigner.expected),
                              LabelRole::Caption))
                ->push(new Label(
                    TR("musig-send-partial-sigs").arg(cosigner.partial_sigs).arg(cosigner.expected),
                    LabelRole::Caption))
                ->pushSpacer();
        // Hidden, the pill keeps its size so every row has the same height.
        auto *pill = catalog::pillSignerSigned();
        auto policy = pill->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        pill->setSizePolicy(policy);
        pill->setVisible(shareIn(status.round, cosigner));
        row->push(pill);
        m_cosigners_column->push(row);
        m_cosigner_rows.append(row);
    }
}

void Send::onSignRoundClicked() {
    m_notice->setVisible(false);
    m_controller->signRound();
}

void Send::onSigningFailed(const QString &message, plugin::SignError kind) {
    if (kind == plugin::SignError::MusigSession) {
        m_failure = Failure::SessionLost;
        m_notice->setTone(catalog::Notice::Tone::Warning);
    } else {
        m_failure = Failure::Sign;
        m_notice->setTone(catalog::Notice::Tone::Error);
    }
    m_notice->setTitle(failureTitle());
    m_notice->setText(message);
    m_notice->setVisible(true);
}

void Send::onSigningCancelled() {
    m_notice->setVisible(false);
}

void Send::onTransactionReady([[maybe_unused]] const QString &tx_hex) {
    m_sign_btn->setVisible(false);
    m_broadcast_btn->setVisible(true);
    m_broadcast_btn->setEnabled(true);
}

// Finalizing is pure computation over the PSBT: retrying gives the same error.
void Send::onFinalizeFailed(const QString &message) {
    m_failure = Failure::Finalize;
    m_notice->setTone(catalog::Notice::Tone::Error);
    m_notice->setTitle(failureTitle());
    m_notice->setText(message);
    m_notice->setVisible(true);
}

void Send::onBroadcastClicked() {
    m_broadcast_btn->setEnabled(false);
    m_cancel_btn->setEnabled(false);
    m_controller->broadcast();
}

void Send::onBroadcastDone(const TxResult &result) {
    m_broadcast_btn->setEnabled(true);
    m_cancel_btn->setEnabled(true);
    if (!result.is_ok) {
        AppController::execModal(new catalog::MessageModal(
            TR("common-error"), TR("confirm-send-failed").arg(toQString(result.error))));
        return;
    }
    AppController::execModal(
        new catalog::MessageModal(TR("confirm-transaction-title"), TR("confirm-send-success"),
                                  catalog::MessageModal::Tone::Success, toQString(result.value)));
    m_signing_card->setVisible(false);
    clearPayments();
    m_coin_table->setSelectedIds({});
    setEditable(true);
    onCoinsSelected({});
}

void Send::onCancelClicked() {
    m_controller->cancelSigning();
    m_notice->setVisible(false);
    m_signing_card->setVisible(false);
    setEditable(true);
}

} // namespace musig::view
