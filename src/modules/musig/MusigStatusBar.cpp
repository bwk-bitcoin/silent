#include "modules/musig/MusigStatusBar.h"

#include "catalog/StatusBarItem.h"
#include "catalog/containers/Separator.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "theme/Palette.h"

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
    setFixedHeight(metric::STATUS_BAR_HEIGHT);
    m_separator = new catalog::Separator(catalog::Separator::Role::Horizontal, this);
    m_electrum_item = new catalog::StatusBarItem(this);
    onReloadUrl();
}

void MusigStatusBar::doConnect() {
    connect(m_electrum_item, &catalog::StatusBarItem::toggled, this,
            &MusigStatusBar::onElectrumToggled, qontrol::UNIQUE);
    connect(m_controller, &AccountBackend::endpointStateChanged, this,
            &MusigStatusBar::onEndpointStateChanged, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::electrumUrlChanged, this,
            &MusigStatusBar::onReloadUrl, qontrol::UNIQUE);
}

void MusigStatusBar::view() {
    auto *contentRow = (new qontrol::Row)->push(m_electrum_item)->pushSpacer();
    contentRow->layout()->setContentsMargins(resolve(Padding::S), resolve(Padding::XXS),
                                             resolve(Padding::S), resolve(Padding::XXS));
    contentRow->layout()->setSpacing(0);

    auto *mainCol = (new qontrol::Column)->push(m_separator)->push(contentRow);
    mainCol->layout()->setContentsMargins(0, 0, 0, 0);
    mainCol->layout()->setSpacing(0);
    setLayout(mainCol->layout());
}

// ===== Slots =====

void MusigStatusBar::onEndpointStateChanged(Endpoint endpoint) {
    if (endpoint != Endpoint::Electrum) {
        return;
    }
    auto state = m_controller->electrumState();
    m_electrum_item->setOn(state == MusigAccountController::ElectrumState::Connected ||
                           state == MusigAccountController::ElectrumState::Connecting);
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
}

void MusigStatusBar::onElectrumToggled(bool checked) {
    if (checked) {
        m_controller->startElectrum();
    } else {
        m_controller->stopElectrum();
    }
    onEndpointStateChanged(Endpoint::Electrum);
}

void MusigStatusBar::onReloadUrl() {
    m_electrum_url = m_controller->electrumUrl();
    // Nothing to connect to without a url: the switch stays off until one is set.
    m_electrum_item->setToggleEnabled(!m_electrum_url.isEmpty());
    onEndpointStateChanged(Endpoint::Electrum);
}
