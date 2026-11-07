#include "StatusBar.h"
#include "AccountController.h"
#include "account/RequestId.h"
#include "catalog/StatusBarItem.h"
#include "catalog/containers/Separator.h"
#include "i18n/Tr.h"
#include "views/BackendErrors.h"
#include "views/utils.h"
#include <common.h>

static auto formatEta(uint64_t secs) -> QString {
    auto hours = secs / 3600;
    auto mins = (secs % 3600) / 60;
    auto s = secs % 60;
    if (hours > 0) {
        return QString("~%1h %2m").arg(hours).arg(mins);
    }
    if (mins > 0) {
        return QString("~%1m %2s").arg(mins).arg(s);
    }
    return QString("~%1s").arg(s);
}

StatusBar::StatusBar(AccountController *controller, QWidget *parent)
    : QWidget(parent),
      m_controller(controller) {
    initUI();
    onReloadUrl();

    // Connect toggle signals
    connect(m_blindbit_item, &catalog::StatusBarItem::toggled, this, &StatusBar::onToggled,
            qontrol::UNIQUE);
    connect(m_electrum_item, &catalog::StatusBarItem::toggled, this, &StatusBar::onElectrumToggled,
            qontrol::UNIQUE);
    connect(m_controller, &AccountBackend::endpointStateChanged, this,
            &StatusBar::onEndpointStateChanged, qontrol::UNIQUE);
    connect(m_controller, &AccountBackend::endpointRequestFinished, this,
            &StatusBar::onEndpointRequestFinished, qontrol::UNIQUE);
}

void StatusBar::initUI() {
    // Blindbit toggle + status
    m_blindbit_item = new catalog::StatusBarItem(this);

    // Vertical line separator
    auto *vline = new catalog::Separator(catalog::Separator::Role::Vertical, this);

    // Electrum toggle + status
    m_electrum_item = new catalog::StatusBarItem(this);

    // Left half: blindbit
    auto *leftRow = (new qontrol::Row)->push(m_blindbit_item)->pushSpacer();

    // Right half: electrum
    auto *rightRow =
        (new qontrol::Row)->pushSpacer(resolve(Spacing::S))->push(m_electrum_item)->pushSpacer();

    // Combine with equal proportions
    auto *contentRow = (new qontrol::Row)->push(leftRow)->push(vline)->push(rightRow);
    contentRow->layout()->setContentsMargins(0, 0, 0, 0);
    contentRow->layout()->setSpacing(0);

    setLayout(contentRow->layout());
}

void StatusBar::onUpdateConnectionState(bool connected) {
    m_blindbit_item->setOn(connected);

    if (connected) {
        m_blindbit_item->setLabel(TR("status-scanning"));
    } else {
        m_blindbit_item->setLabel(TR("status-disconnected"));
    }
}

void StatusBar::onUpdateScanProgress(uint32_t height, uint32_t tip) {
    if (height < tip) {
        auto eta = m_controller->etaSecs();
        QString text = TR("status-scanning-progress").arg(height).arg(tip);
        if (eta > 0) {
            text += QString(" \u2022 %1").arg(formatEta(eta));
        }
        m_blindbit_item->setLabel(text);
    } else {
        // Synced
        m_blindbit_item->setLabel(TR("status-connected-blindbit").arg(m_blindbit_url));
    }
}

void StatusBar::onUpdateWaitingForBlocks(uint32_t tip_height) {
    m_blindbit_item->setLabel(TR("status-synced-watching").arg(tip_height));
}

void StatusBar::onElectrumConnected(const QString &address) {
    m_electrum_item->setOn(true);

    m_electrum_item->setLabel(TR("status-connected-electrum").arg(address));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void StatusBar::onElectrumDisconnected() {
    m_electrum_item->setOn(false);

    m_electrum_item->setLabel(TR("status-electrum-disconnected"));
}

void StatusBar::onReloadUrl() {
    onEndpointStateChanged(Endpoint::Blindbit);
    onEndpointStateChanged(Endpoint::Electrum);
}

void StatusBar::onToggled(bool checked) {
    m_blindbit_req = account::nextRequestId();
    m_controller->setEndpointConnected(m_blindbit_req, Endpoint::Blindbit, checked);
}

void StatusBar::onElectrumToggled(bool checked) {
    m_electrum_req = account::nextRequestId();
    m_controller->setEndpointConnected(m_electrum_req, Endpoint::Electrum, checked);
}

void StatusBar::onEndpointStateChanged(Endpoint endpoint) {
    m_blindbit_url = m_controller->endpointUrl(Endpoint::Blindbit);
    m_electrum_url = m_controller->endpointUrl(Endpoint::Electrum);
    bool busy = m_controller->endpointOperationsBusy();
    m_blindbit_item->setToggleEnabled(!busy && !m_blindbit_url.isEmpty());
    m_electrum_item->setToggleEnabled(!busy && !m_electrum_url.isEmpty() &&
                                      (m_controller->hasSegwit() || m_controller->hasTaproot()));
    auto *item = endpoint == Endpoint::Blindbit ? m_blindbit_item : m_electrum_item;
    auto error = m_controller->endpointError(endpoint);
    auto operation = m_controller->endpointOperation(endpoint);
    if (m_controller->accountLoading()) {
        item->setOn(false);
        item->setLabel(TR("status-connecting"));
        return;
    }
    if (!m_controller->accountLoadError().isEmpty() && !m_controller->hasAccountMetadata()) {
        item->setOn(false);
        item->setLabel(mapBackendErrorSummary(BackendErrorKind::Storage));
        item->setToolTip(m_controller->accountLoadError());
        return;
    }
    if (operation != EndpointOperation::Idle || m_controller->endpointsStopping()) {
        error.clear();
    }
    item->setToolTip(error);
    item->setOn(m_controller->isEndpointRunning(endpoint));
    if (endpoint == Endpoint::Blindbit) {
        onUpdateConnectionState(m_controller->isEndpointRunning(endpoint));
    } else if (m_electrum_url.isEmpty()) {
        item->setLabel(TR("status-not-configured"));
    } else if (!m_controller->hasSegwit() && !m_controller->hasTaproot()) {
        item->setLabel(TR("status-electrum-unused"));
    } else if (m_controller->isEndpointRunning(endpoint)) {
        onElectrumConnected(m_electrum_url);
    } else {
        onElectrumDisconnected();
    }
    if (m_controller->endpointsStopping() || operation == EndpointOperation::Disconnecting) {
        item->setLabel(TR("status-disconnecting"));
    } else if (operation == EndpointOperation::Connecting) {
        item->setLabel(TR("status-connecting"));
    } else if (!error.isEmpty()) {
        item->setLabel(mapBackendErrorSummary(m_controller->endpointErrorKind(endpoint)));
    }
}

void StatusBar::onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                          const EndpointResult &result) {
    auto &req = endpoint == Endpoint::Blindbit ? m_blindbit_req : m_electrum_req;
    if (req_id == 0 || req_id != req) {
        return;
    }
    req = 0;
    onEndpointStateChanged(endpoint);
    if (!result.is_ok && !m_controller->endpointOperationsBusy()) {
        auto *item = endpoint == Endpoint::Blindbit ? m_blindbit_item : m_electrum_item;
        item->setLabel(mapBackendErrorSummary(result.kind));
        item->setToolTip(QString::fromUtf8(result.error.data(), result.error.size()));
    }
}
