#include "account/views/TransactionDetails.h"

#include "AppController.h"
#include "account/Extensions.h"
#include "account/RequestId.h"
#include "account/Same.h"
#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Section.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Amount.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/TxIndicators.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/LabelCell.h"
#include "catalog/panels/send/TxFlow.h"
#include "i18n/Tr.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "theme/Icon.h"

#include <QJsonObject>
#include <QLocale>
#include <QVariant>
#include <Qontrol>
#include <algorithm>
#include <common.h>

namespace account {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

const int INDEX_W = 40; // the design's 3em index column
const int INPUT_REF_HEAD = 8;
const int INPUT_REF_TAIL = 4;
const int OUTPUT_REF_HEAD = 10;
const int OUTPUT_REF_TAIL = 6;

const int TX_DETAILS_W = 820; // the design's transaction modal width

// The statuses the account reports a transaction in.
const QString STATUS_UNCONFIRMED = QStringLiteral("unconfirmed");
const QString STATUS_CONFIRMED_UNVERIFIED = QStringLiteral("confirmed-unverified");
const QString STATUS_VERIFIED = QStringLiteral("verified");
const QString STATUS_VERIFY_FAILED = QStringLiteral("verify-failed");
const QString STATUS_BROADCAST = QStringLiteral("broadcast");
const QString STATUS_FINALIZED = QStringLiteral("finalized");
const QString STATUS_UNKNOWN = QStringLiteral("unknown");

auto shortRef(const QString &value, int head, int tail) -> QString {
    if (value.size() <= head + tail + 1) {
        return value;
    }
    return value.left(head) + QStringLiteral("…") + value.right(tail);
}

auto inputRef(const QString &outpoint) -> QString {
    return shortRef(outpoint.section(':', 0, 0), INPUT_REF_HEAD, INPUT_REF_TAIL) + ':' +
           outpoint.section(':', 1);
}

auto outputKind(const backend::TransactionIo &io) -> catalog::TxFlow::OutputKind {
    return io.owned.value_or(false) ? catalog::TxFlow::OutputKind::Change
                                    : catalog::TxFlow::OutputKind::Recipient;
}

auto sameIo(const backend::TransactionIo &a, const backend::TransactionIo &b) -> bool {
    return a.index == b.index && a.outpoint == b.outpoint && a.address == b.address &&
           a.label == b.label && a.amount == b.amount && a.owned == b.owned;
}

auto sameIos(const QList<backend::TransactionIo> &a, const QList<backend::TransactionIo> &b)
    -> bool {
    return std::ranges::equal(a, b, sameIo);
}

auto sameDetails(const backend::TransactionDetails &a, const backend::TransactionDetails &b)
    -> bool {
    return same(a.transaction, b.transaction) && sameIos(a.inputs, b.inputs) &&
           sameIos(a.outputs, b.outputs) && a.inputTotal == b.inputTotal &&
           a.outputTotal == b.outputTotal && a.virtualSize == b.virtualSize &&
           a.feeRate == b.feeRate && a.warnings == b.warnings;
}

auto secondary(const QString &text, LabelRole role = LabelRole::Body) -> Label * {
    auto *label = new Label(text, role);
    label->setTone(LabelTone::Secondary);
    return label;
}

// A summary line: its name on the left, its value on the right.
auto keyValue(const QString &name, QWidget *value) -> QWidget * {
    return (new qontrol::Row)
        ->spacing(resolve(Spacing::M))
        ->push(secondary(name))
        ->pushSpacer()
        ->push(value);
}

auto strongAmount(uint64_t sats, bool known) -> Label * {
    auto *amount = catalog::amountLabel(sats, known);
    amount->setWeight(QFont::DemiBold);
    amount->setSize(catalog::LabelSize::Lg);
    return amount;
}

// Sent and not seen in a block yet.
auto isUnconfirmed(const QString &status) -> bool {
    return status == STATUS_UNCONFIRMED || status == STATUS_BROADCAST;
}

auto blockText(uint32_t height, int confirmations) -> QString {
    if (confirmations < 1) {
        return TR("tx-status-block").arg(height);
    }
    return TR("tx-status-confirmed").arg(QLocale().toString(confirmations)).arg(height);
}

// The status as the account reports it, nothing inferred from the height alone.
auto statusText(const QString &status, uint32_t height, int confirmations) -> QString {
    if (status == STATUS_UNCONFIRMED) {
        return TR("tx-status-mempool");
    }
    if (status == STATUS_BROADCAST) {
        return TR("tx-status-broadcast");
    }
    if (status == STATUS_FINALIZED) {
        return TR("tx-status-not-broadcast");
    }
    if (status == STATUS_UNKNOWN) {
        return TR("tx-status-not-in-history");
    }
    if (status == STATUS_VERIFIED) {
        return blockText(height, confirmations);
    }
    if (status == STATUS_CONFIRMED_UNVERIFIED) {
        return TR("tx-status-unverified").arg(blockText(height, confirmations));
    }
    if (status == STATUS_VERIFY_FAILED) {
        return TR("tx-status-verify-failed").arg(blockText(height, confirmations));
    }
    // A status this view does not know: shown as the account names it.
    return status;
}

} // namespace

void showTransactionDetails(const Services &services, const QString &txid,
                            const QStringList &notes) {
    AppController::execModal(new TransactionDetails(services, txid, notes));
}

TransactionDetails::TransactionDetails(const Services &services, const QString &txid,
                                       const QStringList &notes)
    : m_services(services),
      m_blockchain(services.blockchain),
      m_labels(services.labels),
      m_txid(txid),
      m_notes(notes) {
    init();
    doConnect();
    view();
    onBackendUpdated();
}

void TransactionDetails::init() {
    m_compact = catalog::compactDates(qontrol::Controller::window());
    initLoading();
}

void TransactionDetails::doConnect() {
    auto *blockchain = m_blockchain.data();
    connect(blockchain, &backend::IBlockchain::coinsChanged, this,
            &TransactionDetails::onBackendUpdated, qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::historyChanged, this,
            &TransactionDetails::onBackendUpdated, qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::stateChanged, this,
            &TransactionDetails::onBackendUpdated, qontrol::UNIQUE);
    connect(m_labels.data(), &backend::ILabels::labelsChanged, this,
            &TransactionDetails::onBackendUpdated, qontrol::UNIQUE);
    connect(this, &TransactionDetails::interactionFinished, this,
            &TransactionDetails::onInteractionFinished,
            Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(blockchain, &backend::IBlockchain::transactionDetailsFetched, this,
            &TransactionDetails::onDetailsFetched, qontrol::UNIQUE);
    connect(blockchain, &backend::IService::valueResult, this, &TransactionDetails::onExtrasResult,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IBlockchain::completed, this, &TransactionDetails::onTipRefreshed,
            qontrol::UNIQUE);
    connect(blockchain, &backend::IService::error, this, &TransactionDetails::onBlockchainError,
            qontrol::UNIQUE);
    connect(m_labels.data(), &backend::ILabels::saved, this, &TransactionDetails::onLabelSaved,
            qontrol::UNIQUE);
    connect(m_labels.data(), &backend::IService::error, this, &TransactionDetails::onLabelError,
            qontrol::UNIQUE);
    // A new price or amount mode changes the fiat on the flow's bands.
    connect(catalog::AmountDisplay::get(), &catalog::AmountDisplay::changed, this,
            &TransactionDetails::onAmountsChanged, qontrol::UNIQUE);
    // The account closing takes its transactions with it.
    connect(blockchain, &QObject::destroyed, this, &TransactionDetails::reject, qontrol::UNIQUE);
    connect(m_labels.data(), &QObject::destroyed, this, &TransactionDetails::reject,
            qontrol::UNIQUE);
}

void TransactionDetails::view() {
    setWindowTitle(TR("tx-details-title"));
    // No ModalWidth token is this wide.
    setFixedWidth(TX_DETAILS_W);
    setWizardHeight(true);
    // What the account knows already shows at once; the fetch refreshes it.
    auto known = m_blockchain->transactionDetails(m_txid);
    if (!known.has_value()) {
        showLoading();
        return;
    }
    requestExtras();
    applyDetails(*known);
}

// ===== Loading view =====

void TransactionDetails::initLoading() {
    m_loading = new catalog::ModalStatus;
}

void TransactionDetails::showLoading() {
    m_loading->setSpinner();
    m_loading->setTitle(TR("tx-details-loading"));
    setBody(m_loading);
}

void TransactionDetails::showFailed(const QString &error) {
    m_loading->setIcon(icon::circleX, IconColor::Error);
    m_loading->setTitle(TR("tx-details-failed"));
    m_loading->setSubtitle(error);
    setBody(m_loading);
}

// ===== Details view =====

auto TransactionDetails::viewDetails() -> QWidget * {
    auto *column = (new qontrol::Column)->spacing(resolve(Spacing::S))->push(headerRow());

    for (const auto &note : m_notes) {
        auto *notice = new catalog::Notice(catalog::Notice::Tone::Warning);
        notice->setText(note);
        column->push(notice);
    }

    for (const auto &warning : m_details.warnings) {
        auto *notice = new catalog::Notice(catalog::Notice::Tone::Warning);
        notice->setText(warning);
        column->push(notice);
    }

    m_label_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_label_error->setTitle(TR("tx-label-failed"));
    m_label_error->setText(m_label_error_text);
    m_label_error->setVisible(!m_label_error_text.isEmpty());

    auto *txid = new Label(m_txid, LabelRole::Mono);
    txid->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *copy = catalog::btnCopyValue();
    copy->setTextToCopy(m_txid);
    auto *txidValue = (new qontrol::Row)->spacing(resolve(Spacing::XXS))->push(txid)->push(copy);

    column->push(m_label_error)
        ->push(keyValue(TR("tx-details-txid"), txidValue))
        ->push(metadataRow())
        ->push(flowCard())
        ->push(ioSections())
        ->push(summary());

    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    scroll->setWidget(column);
    return scroll;
}

void TransactionDetails::showDetails() {
    auto *oldWidget = m_details_widget;
    m_details_widget = viewDetails();
    setWideBody(m_details_widget);
    delete oldWidget;
    updateStatus();
}

void TransactionDetails::refreshDetails() {
    if (!m_stale || !m_label_requests.empty()) {
        return;
    }
    if (m_details_widget != nullptr) {
        for (auto *cell : m_details_widget->findChildren<catalog::LabelCell *>()) {
            if (cell->isEditing()) {
                return;
            }
        }
    }
    m_stale = false;
    showDetails();
}

auto TransactionDetails::headerRow() const -> QWidget * {
    const auto &tx = m_details.transaction;
    auto direction = catalog::txDirection(tx.direction);
    auto *title = new Label(catalog::directionLabel(direction), LabelRole::Body);
    title->setWeight(QFont::DemiBold);

    auto *row = (new qontrol::Row)
                    ->spacing(resolve(Spacing::S))
                    ->push(new catalog::DirectionIcon(direction))
                    ->push(title);
    if (!tx.label.isEmpty()) {
        row->push(secondary(tx.label));
    }
    row->pushSpacer();
    if (isUnconfirmed(tx.status)) {
        row->push(catalog::pillTxUnconfirmed());
    } else if (tx.height.has_value()) {
        auto *date = catalog::dateStamp(tx.timestamp.value_or(0), false, m_compact);
        if (auto *dateLabel = qobject_cast<Label *>(date); dateLabel != nullptr) {
            dateLabel->setTone(LabelTone::Secondary);
        }
        row->push(date);
    }
    return row;
}

auto TransactionDetails::metadataRow() -> QWidget * {
    m_dots = new catalog::ConfirmDots(-1);
    m_status = new Label(LabelRole::Body);
    auto *status = (new qontrol::Row)->spacing(resolve(Spacing::S))->push(m_dots)->push(m_status);

    auto *size =
        new Label(TR("tx-details-vsize").arg(m_details.virtualSize.value_or(0)), LabelRole::Mono);
    auto *version =
        new Label(TR("tx-details-version-lock").arg(m_extras.version).arg(m_extras.lockTime),
                  LabelRole::Mono);

    return (new qontrol::Row)
        ->spacing(resolve(Spacing::L))
        ->push(keyValue(TR("tx-details-status"), status), 2)
        ->push(keyValue(TR("tx-details-size"), size), 1)
        ->push(keyValue(TR("tx-details-version"), version), 1);
}

auto TransactionDetails::flowCard() -> QWidget * {
    m_flow_card = new catalog::TxFlowCard;
    m_flow_card->setDetail(true);
    refreshFlow();
    return m_flow_card;
}

auto TransactionDetails::flowInputs() const -> QList<catalog::TxFlow::InputItem> {
    QList<catalog::TxFlow::InputItem> inputs;
    for (const auto &io : m_details.inputs) {
        QString title = TR("txflow-input-n").arg(io.index);
        if (!io.label.isEmpty()) {
            title += QStringLiteral(" · ") + io.label;
        }
        // Unknown values keep a minimum-width band without inventing an amount.
        inputs.append({.weight = io.amount.has_value() ? static_cast<double>(*io.amount) : 0.0,
                       .title = title,
                       .fiat = io.amount.has_value() ? catalog::AmountDisplay::fiatText(*io.amount)
                                                     : QString(),
                       .addr = io.outpoint,
                       .index = QString::number(io.index),
                       .ref = inputRef(io.outpoint),
                       .value_known = io.amount.has_value()});
    }
    return inputs;
}

auto TransactionDetails::flowOutputs() const -> QList<catalog::TxFlow::OutputItem> {
    QList<catalog::TxFlow::OutputItem> outputs;
    for (const auto &io : m_details.outputs) {
        QString id = outputId(io);
        QString title = TR("txflow-output-n").arg(io.index);
        if (!io.label.isEmpty()) {
            title += QStringLiteral(" · ") + io.label;
        }
        auto value = io.amount.value_or(0);
        outputs.append({.kind = outputKind(io),
                        .weight = static_cast<double>(value),
                        .title = title,
                        .label = io.label,
                        .fiat = catalog::AmountDisplay::fiatText(value),
                        .addr = id,
                        .index = QString::number(io.index),
                        .ref = shortRef(id, OUTPUT_REF_HEAD, OUTPUT_REF_TAIL),
                        .value_known = io.amount.has_value()});
    }
    auto fee = m_details.transaction.fee;
    if (fee.has_value() && *fee > 0) {
        outputs.append({.kind = catalog::TxFlow::OutputKind::Fee,
                        .weight = static_cast<double>(*fee),
                        .title = TR("txflow-fee-title"),
                        .fiat = catalog::AmountDisplay::fiatText(*fee),
                        .addr = TR("txflow-fee-addr")});
    }
    return outputs;
}

auto TransactionDetails::outputId(const backend::TransactionIo &io) const -> QString {
    return io.address.isEmpty() ? m_extras.scripts.value(io.index) : io.address;
}

void TransactionDetails::refreshFlow() {
    if (m_flow_card == nullptr) {
        return;
    }
    m_flow_card->setInputs(flowInputs());
    m_flow_card->setOutputs(flowOutputs());
}

auto TransactionDetails::ioSections() const -> QWidget * {
    auto *inputs = new catalog::Section(TR("tx-details-inputs"));
    inputs->header()->setCount(QString::number(m_details.inputs.size()));
    inputs->header()->setDense(true);
    inputs->setContent(ioTable(m_details.inputs, true));

    auto *outputs = new catalog::Section(TR("tx-details-outputs"));
    outputs->header()->setCount(QString::number(m_details.outputs.size()));
    outputs->header()->setDense(true);
    outputs->setContent(ioTable(m_details.outputs, false));

    return (new qontrol::Row)
        ->spacing(resolve(Spacing::L))
        ->push(inputs, 1, Qt::AlignTop)
        ->push(outputs, 1, Qt::AlignTop);
}

auto TransactionDetails::ioTable(const QList<backend::TransactionIo> &ios, bool inputs) const
    -> QWidget * {
    auto *table = new catalog::Table;
    table->setHeaderVisible(false);
    table->addColumn({.width = INDEX_W});
    table->addColumn({.grow = true});
    table->addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    int valueWidth = 0;
    QList<Label *> amounts;
    for (const auto &io : ios) {
        auto *amount = catalog::amountLabel(io.amount.value_or(0), io.amount.has_value());
        amount->ensurePolished();
        valueWidth = qMax(valueWidth, amount->sizeHint().width());
        amounts.append(amount);
    }
    table->addColumn({.width = valueWidth, .alignment = Qt::AlignRight});
    int index = 0;
    for (const auto &io : ios) {
        const auto &outpoint = io.outpoint;
        auto swatch =
            inputs ? catalog::TxLegend::Swatch::Self : catalog::TxLegend::swatchFor(outputKind(io));

        auto *cell = new catalog::LabelCell(io.label);
        cell->setRetryUnchanged(true);
        cell->setProperty("outpoint", outpoint);
        connect(cell, &catalog::LabelCell::edited, this, &TransactionDetails::onIoLabelEdited,
                qontrol::UNIQUE);
        connect(cell, &catalog::LabelCell::interactionFinished, this,
                &TransactionDetails::interactionFinished, qontrol::UNIQUE);

        table->addWidgetRow(outpoint, {new catalog::TxLegend(swatch, QString::number(io.index)),
                                       cell, cell->takeEditButton(), amounts[index++]});
    }
    return table;
}

auto TransactionDetails::summary() const -> QWidget * {
    const auto &tx = m_details.transaction;
    auto direction = catalog::txDirection(tx.direction);
    auto *column = new qontrol::Column;
    QString totalLabel;
    switch (direction) {
    case catalog::TxDirection::Outgoing:
    case catalog::TxDirection::Self:
        totalLabel = tr("Debited");
        break;
    case catalog::TxDirection::Incoming:
        totalLabel = TR("tx-details-received");
        break;
    case catalog::TxDirection::Unknown:
        totalLabel = tr("Account change");
        break;
    }

    QWidget *fee = catalog::amountLabel(tx.fee.value_or(0), tx.fee.has_value());
    if (tx.fee.has_value()) {
        fee = (new qontrol::Row)
                  ->spacing(resolve(Spacing::XS))
                  ->push(fee)
                  ->push(new Label(TR("tx-details-fee-rate")
                                       .arg(QString::number(m_details.feeRate.value_or(0), 'f', 1)),
                                   LabelRole::Mono));
    }
    auto *bottom =
        (new qontrol::Row)
            ->spacing(resolve(Spacing::L))
            ->push(keyValue(tr("Transaction fees"), fee), 1)
            ->push(keyValue(totalLabel, strongAmount(tx.amount.value_or(0), tx.amount.has_value())),
                   1);
    return column->push(new catalog::Separator)->push(bottom);
}

void TransactionDetails::applyDetails(const backend::TransactionDetails &details) {
    m_stale = m_stale || m_details_widget == nullptr || !sameDetails(m_details, details);
    m_details = details;
    updateStatus();
    refreshDetails();
    if (m_status != nullptr) {
        m_status->setToolTip({});
    }
    if (tip() == 0 && m_details.transaction.height.has_value()) {
        requestTip();
    }
}

void TransactionDetails::requestExtras() {
    if (m_blockchain == nullptr || m_extras_request != 0) {
        return;
    }
    m_extras_request = nextRequestId();
    // The built-in providers answer during the call.
    m_blockchain->value(m_extras_request, extension::TRANSACTION_EXTRAS, QJsonValue(m_txid));
}

void TransactionDetails::requestTip() {
    if (m_blockchain == nullptr || m_tip_request != 0) {
        return;
    }
    for (const auto &connection : m_blockchain->state().connections) {
        if (connection.connected) {
            m_tip_request = nextRequestId();
            m_blockchain->refreshTip(m_tip_request, connection.id);
            return;
        }
    }
}

// The tip the provider reported with the transaction, else the tip of a connected server.
auto TransactionDetails::tip() const -> uint32_t {
    if (m_extras.chainTip.has_value()) {
        return static_cast<uint32_t>(*m_extras.chainTip);
    }
    return m_blockchain == nullptr ? 0 : stateTip(m_blockchain->state());
}

void TransactionDetails::updateStatus() {
    if (m_status == nullptr) {
        return;
    }
    const auto &tx = m_details.transaction;
    auto height = static_cast<uint32_t>(tx.height.value_or(0));
    int confirmations = catalog::txConfirmations(height, tip());
    m_dots->setConfirmations(confirmations);
    m_dots->setVisible(tx.status == STATUS_VERIFIED && height > 0 && confirmations >= 1);
    auto tone = LabelTone::Default;
    if (tx.status == STATUS_VERIFY_FAILED) {
        tone = LabelTone::Error;
    } else if (tx.status == STATUS_CONFIRMED_UNVERIFIED) {
        tone = LabelTone::Warning;
    }
    m_status->setTone(tone);
    m_status->setText(statusText(tx.status, height, confirmations));
}

void TransactionDetails::setIoLabel(const QString &outpoint, const QString &label) {
    for (auto *ios : {&m_details.inputs, &m_details.outputs}) {
        for (auto &io : *ios) {
            if (io.outpoint == outpoint) {
                io.label = label;
            }
        }
    }
}

// ===== Slots =====

void TransactionDetails::onBackendUpdated() {
    if (m_blockchain == nullptr) {
        return;
    }
    if (m_details_request != 0) {
        m_refresh_pending = true;
        return;
    }
    m_refresh_pending = false;
    m_details_request = nextRequestId();
    // Registered before the request: the answer may come during the call.
    m_blockchain->fetchTransactionDetails(m_details_request, m_txid);
}

void TransactionDetails::onInteractionFinished() {
    refreshDetails();
}

void TransactionDetails::onDetailsFetched(backend::RequestId request_id,
                                          const backend::TransactionDetails &details) {
    if (request_id != m_details_request) {
        return;
    }
    m_details_request = 0;
    requestExtras();
    applyDetails(details);
    if (m_refresh_pending) {
        onBackendUpdated();
    }
}

void TransactionDetails::onBlockchainError(backend::RequestId request_id, const QString &message) {
    if (request_id == m_extras_request) {
        // A provider without the extension: the details go without it.
        m_extras_request = 0;
        return;
    }
    if (request_id == m_tip_request) {
        m_tip_request = 0;
        if (m_status != nullptr) {
            m_status->setToolTip(message);
        }
        return;
    }
    if (request_id != m_details_request) {
        return;
    }
    m_details_request = 0;
    if (m_details_widget == nullptr) {
        showFailed(message);
    } else {
        m_status->setToolTip(message);
    }
    if (m_refresh_pending) {
        onBackendUpdated();
    }
}

void TransactionDetails::onExtrasResult(backend::RequestId request_id, const QJsonValue &payload) {
    if (request_id != m_extras_request) {
        return;
    }
    m_extras_request = 0;
    auto object = payload.toObject();
    Extras extras{.version = object.value(QStringLiteral("version")).toInteger(),
                  .lockTime = object.value(QStringLiteral("lockTime")).toInteger()};
    if (auto chainTip = object.value(QStringLiteral("chainTip")); chainTip.isDouble()) {
        extras.chainTip = static_cast<quint64>(chainTip.toInteger());
    }
    auto scripts = object.value(QStringLiteral("scripts")).toObject();
    for (auto it = scripts.constBegin(); it != scripts.constEnd(); ++it) {
        extras.scripts.insert(it.key().toUInt(), it.value().toString());
    }
    bool changed = extras.version != m_extras.version || extras.lockTime != m_extras.lockTime ||
                   extras.scripts != m_extras.scripts;
    m_extras = extras;
    updateStatus();
    if (changed && m_details_widget != nullptr) {
        m_stale = true;
        refreshDetails();
    }
}

void TransactionDetails::onTipRefreshed(backend::RequestId request_id) {
    if (request_id != m_tip_request) {
        return;
    }
    m_tip_request = 0;
    if (m_status != nullptr) {
        m_status->setToolTip({});
    }
    updateStatus();
}

void TransactionDetails::onLabelSaved(backend::RequestId request_id) {
    if (!m_label_requests.contains(request_id)) {
        return;
    }
    auto pending = m_label_requests.take(request_id);
    m_label_error_text.clear();
    emit interactionFinished();
    setIoLabel(pending.outpoint, pending.label);
    m_label_error->setVisible(false);
    refreshFlow();
}

void TransactionDetails::onLabelError(backend::RequestId request_id, const QString &message) {
    if (!m_label_requests.contains(request_id)) {
        return;
    }
    m_label_requests.remove(request_id);
    m_label_error_text = message;
    emit interactionFinished();
    m_label_error->setText(message);
    m_label_error->setVisible(true);
}

void TransactionDetails::onIoLabelEdited(const QString &label) {
    auto *cell = qobject_cast<catalog::LabelCell *>(sender());
    if (cell == nullptr || m_labels == nullptr) {
        return;
    }
    auto outpoint = cell->property("outpoint").toString();
    auto requestId = nextRequestId();
    // Before the request: the answer may come during the call.
    m_label_requests.insert(requestId, {.outpoint = outpoint, .label = label});
    m_labels->setLabel(requestId, {.kind = backend::LabelKind::Outpoint, .id = outpoint}, label);
}

void TransactionDetails::onAmountsChanged() {
    refreshFlow();
}

} // namespace account
