#include "account/views/TransactionDetails.h"

#include "AppController.h"
#include "account/Equality.h"
#include "account/RequestId.h"
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
#include "theme/Icon.h"

#include <QLocale>
#include <QVariant>
#include <Qontrol>
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

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

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

// The address an output pays, its script when it has none (OP_RETURN).
auto outputId(const RustTxIo &io) -> QString {
    return io.address.empty() ? toQString(io.script) : toQString(io.address);
}

auto outputKind(const RustTxIo &io) -> catalog::TxFlow::OutputKind {
    return io.is_mine ? catalog::TxFlow::OutputKind::Change
                      : catalog::TxFlow::OutputKind::Recipient;
}

auto sameIos(const rust::Vec<RustTxIo> &a, const rust::Vec<RustTxIo> &b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].outpoint != b[i].outpoint || a[i].label != b[i].label ||
            a[i].value != b[i].value || a[i].value_known != b[i].value_known ||
            a[i].is_mine != b[i].is_mine || a[i].address != b[i].address ||
            a[i].script != b[i].script || a[i].index != b[i].index) {
            return false;
        }
    }
    return true;
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

void setIoLabel(rust::Vec<RustTxIo> &ios, const QString &outpoint, const QString &label) {
    for (auto &io : ios) {
        if (toQString(io.outpoint) == outpoint) {
            io.label = rust::String(label.toStdString());
        }
    }
}

} // namespace

void showTransactionDetails(AccountBackend *backend, const QString &txid,
                            const QStringList &notes) {
    AppController::execModal(new TransactionDetails(backend, txid, notes));
}

TransactionDetails::TransactionDetails(AccountBackend *backend, const QString &txid,
                                       const QStringList &notes)
    : m_backend(backend),
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
    auto *backend = m_backend.data();
    connect(backend, &AccountBackend::updateCoins, this, &TransactionDetails::onBackendUpdated,
            qontrol::UNIQUE);
    connect(backend, &AccountBackend::scanProgress, this, &TransactionDetails::onBackendUpdated,
            qontrol::UNIQUE);
    connect(backend, &AccountBackend::endpointStateChanged, this,
            &TransactionDetails::onBackendUpdated, qontrol::UNIQUE);
    connect(backend, &AccountBackend::labelsChanged, this, &TransactionDetails::onBackendUpdated,
            qontrol::UNIQUE);
    connect(this, &TransactionDetails::interactionFinished, this,
            &TransactionDetails::onInteractionFinished,
            Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(backend, &AccountBackend::txDetailsReady, this, &TransactionDetails::onTxDetailsReady,
            qontrol::UNIQUE);
    connect(backend, &AccountBackend::endpointTipReady, this,
            &TransactionDetails::onEndpointTipReady, qontrol::UNIQUE);
    connect(backend, &AccountBackend::labelEdited, this, &TransactionDetails::onLabelEdited,
            qontrol::UNIQUE);
    // A new price or amount mode changes the fiat on the flow's bands.
    connect(catalog::AmountDisplay::get(), &catalog::AmountDisplay::changed, this,
            &TransactionDetails::onAmountsChanged, qontrol::UNIQUE);
    // The account closing takes its transactions with it.
    connect(backend, &QObject::destroyed, this, &TransactionDetails::reject, qontrol::UNIQUE);
}

void TransactionDetails::view() {
    setWindowTitle(TR("tx-details-title"));
    // No ModalWidth token is this wide.
    setFixedWidth(TX_DETAILS_W);
    setWizardHeight(true);
    showLoading();
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

    QString warning = toQString(m_details.warning);
    if (!warning.isEmpty()) {
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
    auto direction = catalog::txDirection(toQString(m_details.tx.direction));
    auto *title = new Label(catalog::directionLabel(direction), LabelRole::Body);
    title->setWeight(QFont::DemiBold);

    auto *row = (new qontrol::Row)
                    ->spacing(resolve(Spacing::S))
                    ->push(new catalog::DirectionIcon(direction))
                    ->push(title);
    QString label = toQString(m_details.tx.label);
    if (!label.isEmpty()) {
        row->push(secondary(label));
    }
    row->pushSpacer();
    if (isUnconfirmed(toQString(m_details.tx.status))) {
        row->push(catalog::pillTxUnconfirmed());
    } else if (m_details.tx.height > 0) {
        auto *date = catalog::dateStamp(m_details.tx.timestamp, false, m_compact);
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

    auto *size = new Label(TR("tx-details-vsize").arg(m_details.vsize), LabelRole::Mono);
    auto *version =
        new Label(TR("tx-details-version-lock").arg(m_details.version).arg(m_details.lock_time),
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
        QString outpoint = toQString(io.outpoint);
        QString title = TR("txflow-input-n").arg(io.index);
        if (!io.label.empty()) {
            title += QStringLiteral(" · ") + toQString(io.label);
        }
        // Unknown values keep a minimum-width band without inventing an amount.
        inputs.append(
            {.weight = io.value_known ? static_cast<double>(io.value) : 0.0,
             .title = title,
             .fiat = io.value_known ? catalog::AmountDisplay::fiatText(io.value) : QString(),
             .addr = outpoint,
             .index = QString::number(io.index),
             .ref = inputRef(outpoint),
             .value_known = io.value_known});
    }
    return inputs;
}

auto TransactionDetails::flowOutputs() const -> QList<catalog::TxFlow::OutputItem> {
    QList<catalog::TxFlow::OutputItem> outputs;
    for (const auto &io : m_details.outputs) {
        QString id = outputId(io);
        QString label = toQString(io.label);
        QString title = TR("txflow-output-n").arg(io.index);
        if (!label.isEmpty()) {
            title += QStringLiteral(" · ") + label;
        }
        outputs.append({.kind = outputKind(io),
                        .weight = static_cast<double>(io.value),
                        .title = title,
                        .label = label,
                        .fiat = catalog::AmountDisplay::fiatText(io.value),
                        .addr = id,
                        .index = QString::number(io.index),
                        .ref = shortRef(id, OUTPUT_REF_HEAD, OUTPUT_REF_TAIL),
                        .value_known = io.value_known});
    }
    if (m_details.fee_known && m_details.fee > 0) {
        outputs.append({.kind = catalog::TxFlow::OutputKind::Fee,
                        .weight = static_cast<double>(m_details.fee),
                        .title = TR("txflow-fee-title"),
                        .fiat = catalog::AmountDisplay::fiatText(m_details.fee),
                        .addr = TR("txflow-fee-addr")});
    }
    return outputs;
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

auto TransactionDetails::ioTable(const rust::Vec<RustTxIo> &ios, bool inputs) const -> QWidget * {
    auto *table = new catalog::Table;
    table->setHeaderVisible(false);
    table->addColumn({.width = INDEX_W});
    table->addColumn({.grow = true});
    table->addColumn({.width = resolve(BtnWidth::Icon), .icon = true});
    int valueWidth = 0;
    QList<Label *> amounts;
    for (const auto &io : ios) {
        auto *amount = catalog::amountLabel(io.value, io.value_known);
        amount->ensurePolished();
        valueWidth = qMax(valueWidth, amount->sizeHint().width());
        amounts.append(amount);
    }
    table->addColumn({.width = valueWidth, .alignment = Qt::AlignRight});
    int index = 0;
    for (const auto &io : ios) {
        QString outpoint = toQString(io.outpoint);
        QString label = toQString(io.label);
        auto swatch =
            inputs ? catalog::TxLegend::Swatch::Self : catalog::TxLegend::swatchFor(outputKind(io));

        auto *cell = new catalog::LabelCell(label);
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
    auto direction = catalog::txDirection(toQString(m_details.tx.direction));
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

    QWidget *fee = catalog::amountLabel(m_details.fee, m_details.fee_known);
    if (m_details.fee_known) {
        fee = (new qontrol::Row)
                  ->spacing(resolve(Spacing::XS))
                  ->push(fee)
                  ->push(new Label(
                      TR("tx-details-fee-rate").arg(QString::number(m_details.fee_rate, 'f', 1)),
                      LabelRole::Mono));
    }
    auto *bottom = (new qontrol::Row)
                       ->spacing(resolve(Spacing::L))
                       ->push(keyValue(tr("Transaction fees"), fee), 1)
                       ->push(keyValue(totalLabel, strongAmount(m_details.tx.amount,
                                                                m_details.tx.amount_known)),
                              1);
    return column->push(new catalog::Separator)->push(bottom);
}

void TransactionDetails::requestTip() {
    if (m_backend == nullptr || m_tip_request != 0) {
        return;
    }
    for (auto endpoint : m_backend->endpoints()) {
        if (m_backend->isEndpointRunning(endpoint)) {
            m_tip_request = nextRequestId();
            m_backend->requestEndpointTip(m_tip_request, endpoint);
            return;
        }
    }
}

void TransactionDetails::updateStatus() {
    if (m_status == nullptr) {
        return;
    }
    const auto &tx = m_details.tx;
    int confirmations = catalog::txConfirmations(tx.height, m_tip);
    m_dots->setConfirmations(confirmations);
    auto status = toQString(tx.status);
    m_dots->setVisible(status == STATUS_VERIFIED && tx.height > 0 && confirmations >= 1);
    auto tone = LabelTone::Default;
    if (status == STATUS_VERIFY_FAILED) {
        tone = LabelTone::Error;
    } else if (status == STATUS_CONFIRMED_UNVERIFIED) {
        tone = LabelTone::Warning;
    }
    m_status->setTone(tone);
    m_status->setText(statusText(status, tx.height, confirmations));
}

// ===== Slots =====

void TransactionDetails::onBackendUpdated() {
    if (m_backend == nullptr) {
        return;
    }
    if (m_details_request != 0) {
        m_refresh_pending = true;
        return;
    }
    m_refresh_pending = false;
    m_details_request = nextRequestId();
    m_backend->requestTxDetails(m_details_request, m_txid);
}

void TransactionDetails::onInteractionFinished() {
    refreshDetails();
}

void TransactionDetails::onTxDetailsReady(uint64_t req_id, const RustTxDetails &details) {
    if (req_id != m_details_request) {
        return;
    }
    m_details_request = 0;
    if (!details.is_ok) {
        if (m_details_widget == nullptr) {
            showFailed(toQString(details.error));
        } else {
            m_status->setToolTip(toQString(details.error));
        }
        if (m_refresh_pending) {
            onBackendUpdated();
        }
        return;
    }
    m_stale = m_stale || m_details_widget == nullptr || m_details.tx != details.tx ||
              !sameIos(m_details.inputs, details.inputs) ||
              !sameIos(m_details.outputs, details.outputs) ||
              m_details.warning != details.warning || m_details.fee != details.fee ||
              m_details.fee_known != details.fee_known || m_details.fee_rate != details.fee_rate ||
              m_details.vsize != details.vsize || m_details.version != details.version ||
              m_details.lock_time != details.lock_time;
    m_details = details;
    m_tip = details.chain_tip_known ? details.chain_tip : 0;
    updateStatus();
    refreshDetails();
    m_status->setToolTip({});
    if (!details.chain_tip_known && details.tx.height > 0) {
        requestTip();
    }
    if (m_refresh_pending) {
        onBackendUpdated();
    }
}

void TransactionDetails::onEndpointTipReady(uint64_t req_id, [[maybe_unused]] Endpoint endpoint,
                                            const ElectrumTipResult &result) {
    if (req_id != m_tip_request) {
        return;
    }
    m_tip_request = 0;
    if (!result.is_ok) {
        m_status->setToolTip(toQString(result.error));
        return;
    }
    m_tip = result.height;
    m_status->setToolTip({});
    updateStatus();
}

void TransactionDetails::onLabelEdited(uint64_t req_id, bool accepted, const QString &error) {
    if (!m_label_requests.contains(req_id)) {
        return;
    }
    auto pending = m_label_requests.take(req_id);
    m_label_error_text = accepted ? QString() : error;
    emit interactionFinished();
    if (accepted) {
        setIoLabel(m_details.inputs, pending.outpoint, pending.label);
        setIoLabel(m_details.outputs, pending.outpoint, pending.label);
        m_label_error->setVisible(false);
        refreshFlow();
        return;
    }
    m_label_error->setText(error);
    m_label_error->setVisible(true);
}

void TransactionDetails::onIoLabelEdited(const QString &label) {
    auto *cell = qobject_cast<catalog::LabelCell *>(sender());
    if (cell == nullptr || m_backend == nullptr) {
        return;
    }
    auto outpoint = cell->property("outpoint").toString();
    auto reqId = nextRequestId();
    m_label_requests.insert(reqId, {.outpoint = outpoint, .label = label});
    m_backend->editOutpointLabel(reqId, outpoint, label);
}

void TransactionDetails::onAmountsChanged() {
    refreshFlow();
}

} // namespace account
