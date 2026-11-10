#include "modules/multikey/MusigStatusBar.h"

#include "account/RequestId.h"
#include "catalog/StatusBarItem.h"
#include "i18n/Tr.h"
#include "modules/multikey/MusigAccountController.h"
#include "views/BackendErrors.h"
#include "views/utils.h"

#include <Qontrol>
#include <common.h>

MusigStatusBar::MusigStatusBar(MusigAccountController *controller, QWidget *parent)
    : QWidget(parent),
      m_controller(controller) {
    init();
    doConnect();
    view();
}

void MusigStatusBar::init() {
    m_electrum_item = new catalog::StatusBarItem(this);
    onReloadUrl();
}

void MusigStatusBar::doConnect() {
    connect(m_electrum_item, &catalog::StatusBarItem::toggled, this,
            &MusigStatusBar::onElectrumToggled, qontrol::UNIQUE);
    connect(m_controller, &AccountLifecycle::endpointStateChanged, this,
            &MusigStatusBar::onEndpointStateChanged, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::electrumUrlChanged, this,
            &MusigStatusBar::onReloadUrl, qontrol::UNIQUE);
    connect(m_controller, &AccountLifecycle::endpointRequestFinished, this,
            &MusigStatusBar::onEndpointRequestFinished, qontrol::UNIQUE);
}

void MusigStatusBar::view() {
    auto *contentRow = (new qontrol::Row)->push(m_electrum_item)->pushSpacer();
    contentRow->layout()->setContentsMargins(0, 0, 0, 0);
    contentRow->layout()->setSpacing(0);
    setLayout(contentRow->layout());
}

// ===== Slots =====

void MusigStatusBar::onEndpointStateChanged(Endpoint endpoint) {
    if (endpoint != Endpoint::Electrum) {
        return;
    }
    auto state = m_controller->electrumState();
    m_electrum_url = m_controller->endpointUrl(endpoint);
    m_electrum_item->setToggleEnabled(!m_electrum_url.isEmpty() &&
                                      !m_controller->endpointOperationsBusy());
    m_electrum_item->setOn(m_controller->isEndpointRunning(endpoint));
    if (!m_controller->accountLoadError().isEmpty() && !m_controller->hasAccountMetadata()) {
        m_electrum_item->setLabel(mapBackendErrorSummary(BackendErrorKind::Storage));
        m_electrum_item->setToolTip(m_controller->accountLoadError());
        return;
    }
    switch (state) {
    case MusigAccountController::ElectrumState::Disconnected:
        m_electrum_item->setLabel(m_electrum_url.isEmpty() ? TR("status-not-configured")
                                                           : TR("status-electrum-disconnected"));
        break;
    case MusigAccountController::ElectrumState::Connecting:
        m_electrum_item->setLabel(TR("status-connecting"));
        break;
    case MusigAccountController::ElectrumState::Connected:
        m_electrum_item->setLabel(TR("status-connected-electrum").arg(m_electrum_url));
        break;
    case MusigAccountController::ElectrumState::Disconnecting:
        m_electrum_item->setLabel(TR("status-disconnecting"));
        break;
    }
    auto error = m_controller->endpointError(endpoint);
    m_electrum_item->setToolTip(error);
    if (!error.isEmpty() && !m_controller->endpointOperationsBusy()) {
        m_electrum_item->setLabel(
            mapBackendErrorSummary(m_controller->endpointErrorKind(endpoint)));
    }
}

void MusigStatusBar::onElectrumToggled(bool checked) {
    m_endpoint_req = account::nextRequestId();
    m_controller->setEndpointConnected(m_endpoint_req, Endpoint::Electrum, checked);
}

void MusigStatusBar::onReloadUrl() {
    onEndpointStateChanged(Endpoint::Electrum);
}

void MusigStatusBar::onEndpointRequestFinished(uint64_t req_id, Endpoint endpoint,
                                               const EndpointResult &result) {
    if (endpoint != Endpoint::Electrum || req_id == 0 || req_id != m_endpoint_req) {
        return;
    }
    m_endpoint_req = 0;
    onEndpointStateChanged(endpoint);
    if (!result.is_ok && !m_controller->endpointOperationsBusy()) {
        m_electrum_item->setLabel(mapBackendErrorSummary(result.kind));
        m_electrum_item->setToolTip(QString::fromUtf8(result.error.data(), result.error.size()));
    }
}
