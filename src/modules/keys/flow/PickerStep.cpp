#include "PickerStep.h"

#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/feedback/EmptyState.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace keys::flow {

namespace {

constexpr const char *ROW_INDEX = "flowRowIndex";

auto statusPill(SignerStatus status) -> catalog::Badge * {
    switch (status) {
    case SignerStatus::First:
        return catalog::pillSignerFirst();
    case SignerStatus::Ready:
        return catalog::pillSignerReady();
    case SignerStatus::Signed:
        return catalog::pillSignerSigned();
    case SignerStatus::Waiting:
        return catalog::pillSignerWaiting();
    case SignerStatus::Disconnected:
        return catalog::pillSignerDisconnected();
    case SignerStatus::NoHot:
        return catalog::pillSignerNoHot();
    case SignerStatus::NoRoute:
        return catalog::pillSignerNoRoute();
    case SignerStatus::Unregistered:
        return catalog::pillSignerUnregistered();
    }
    return catalog::pillSignerNoRoute();
}

auto statusSub(const FlowRow &row) -> QString {
    switch (row.status) {
    case SignerStatus::First:
        return TR("flow-signer-first-sub");
    case SignerStatus::Ready:
        return TR("flow-signer-ready-sub").arg(catalog::routeLabel(row.route));
    case SignerStatus::Signed:
        return TR("flow-signer-signed-sub");
    case SignerStatus::Waiting:
        return TR("flow-signer-waiting-sub");
    case SignerStatus::Disconnected:
        return TR("flow-signer-disconnected-sub")
            .arg(row.device.isEmpty() ? TR("flow-the-device") : row.device);
    case SignerStatus::NoHot:
        return TR("flow-signer-no-hot-sub");
    case SignerStatus::NoRoute:
        return TR("flow-signer-no-route-sub");
    case SignerStatus::Unregistered:
        return TR("flow-signer-unregistered-sub");
    }
    return QString();
}

} // namespace

auto FlowRow::usable() const -> bool {
    return status == SignerStatus::First || status == SignerStatus::Ready;
}

PickerStep::PickerStep(QWidget *parent) : QWidget(parent) {
    m_column = (new qontrol::Column)->spacing(resolve(Spacing::M));
    (new qontrol::Column)->push(m_column)->into(this);
}

void PickerStep::setRows(const QString &lead, const QList<FlowRow> &rows,
                         const QList<OtherDevice> &others, const QString &empty,
                         const QString &other_sub) {
    // clear() also resets the layout spacing
    m_column->clear();
    m_column->spacing(resolve(Spacing::M));

    if (rows.isEmpty()) {
        m_column->push(new catalog::EmptyState(icon::key, TR("flow-no-signer-title"), empty));
    } else {
        if (!lead.isEmpty()) {
            auto *leadLabel = new catalog::Label(lead, catalog::LabelRole::Body);
            leadLabel->setTone(catalog::LabelTone::Secondary);
            leadLabel->setWordWrap(true);
            m_column->push(leadLabel);
        }
        auto *rowsColumn = (new qontrol::Column)->spacing(resolve(Spacing::S));
        for (int index = 0; index < rows.size(); ++index) {
            rowsColumn->push(buildRow(rows.at(index), index));
        }
        m_column->push(rowsColumn);
    }

    if (!others.isEmpty()) {
        auto *othersColumn =
            (new qontrol::Column)
                ->spacing(resolve(Spacing::S))
                ->push(new catalog::Label(TR("flow-other-devices"), catalog::LabelRole::Eyebrow));
        for (const auto &device : others) {
            othersColumn->push(buildOtherRow(device, other_sub));
        }
        m_column->push(othersColumn);
    }
}

void PickerStep::onRowClicked() {
    auto *row = sender();
    if (row == nullptr) {
        return;
    }
    emit rowPicked(row->property(ROW_INDEX).toInt());
}

auto PickerStep::buildRow(const FlowRow &row, int index) const -> catalog::SelectRow * {
    auto *selectRow = new catalog::SelectRow;
    selectRow->setIcon(row.route != KeyRouteKind::Unknown ? catalog::routeIcon(row.route)
                                                          : icon::key);
    selectRow->setTitle(row.alias.isEmpty() ? TR("flow-unknown-key") : row.alias);
    selectRow->setSub(statusSub(row));
    selectRow->setMeta(row.fingerprint);
    selectRow->setStatus(statusPill(row.status));
    selectRow->setStatusWidth(resolve(PillWidth::L));
    if (row.usable()) {
        selectRow->setProperty(ROW_INDEX, index);
        connect(selectRow, &catalog::SelectRow::clicked, this, &PickerStep::onRowClicked,
                qontrol::UNIQUE);
    } else {
        selectRow->setClickable(false);
        selectRow->setEnabled(row.status == SignerStatus::Signed);
    }
    return selectRow;
}

auto PickerStep::buildOtherRow(const OtherDevice &device, const QString &sub)
    -> catalog::SelectRow * {
    auto *selectRow = new catalog::SelectRow;
    selectRow->setIcon(icon::usb);
    selectRow->setTitle(device.name);
    selectRow->setSub(sub);
    selectRow->setMeta(device.fingerprint);
    selectRow->setClickable(false);
    selectRow->setEnabled(false);
    return selectRow;
}

} // namespace keys::flow
