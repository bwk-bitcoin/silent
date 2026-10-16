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

void MusigStatusBar::onElectrumConnected(const QString &address) {
    m_electrum_item->setOn(true);
    m_electrum_item->setLabel(TR("status-connected-electrum").arg(address));
}

void MusigStatusBar::onElectrumDisconnected() {
    m_electrum_item->setOn(false);
    m_electrum_item->setLabel(TR("status-electrum-disconnected"));
}

void MusigStatusBar::onElectrumToggled(bool checked) {
    if (checked) {
        m_electrum_item->setLabel(TR("status-connecting"));
        m_controller->startElectrum();
    } else {
        m_electrum_item->setLabel(TR("status-disconnecting"));
        m_controller->stopElectrum();
    }
}

void MusigStatusBar::onReloadUrl() {
    m_electrum_url = m_controller->electrumUrl();
    if (m_electrum_url.isEmpty()) {
        m_electrum_item->setLabel(TR("status-not-configured"));
    } else {
        m_electrum_item->setLabel(TR("status-connecting"));
    }
}
