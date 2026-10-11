#include "StatusBar.h"
#include "AccountController.h"
#include "catalog/StatusBarItem.h"
#include "catalog/containers/Separator.h"
#include "i18n/Tr.h"
#include "views/utils.h"
#include <QBrush>
#include <QDebug>
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
    loadBlindbitUrl();
    loadElectrumUrl();

    // Set initial blindbit state
    m_connected = m_controller->isScannerRunning();
    onUpdateConnectionState(m_connected);

    // Set initial electrum state (sub-accounts auto-start on construction). Only the
    // segwit and taproot sub-accounts connect: without them nothing will.
    if (m_electrum_url.isEmpty()) {
        m_electrum_item->setLabel(TR("status-not-configured"));
    } else if (!m_controller->hasSegwit() && !m_controller->hasTaproot()) {
        m_electrum_item->setLabel(TR("status-electrum-unused"));
    } else {
        m_electrum_item->setLabel(TR("status-connecting"));
    }

    // Connect toggle signals
    connect(m_blindbit_item, &catalog::StatusBarItem::toggled, this, &StatusBar::onToggled,
            qontrol::UNIQUE);
    connect(m_electrum_item, &catalog::StatusBarItem::toggled, this, &StatusBar::onElectrumToggled,
            qontrol::UNIQUE);
}

void StatusBar::initUI() {
    setFixedHeight(metric::STATUS_BAR_HEIGHT);

    // Top separator line
    auto *separator = new catalog::Separator(catalog::Separator::Role::Horizontal, this);

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
    contentRow->layout()->setContentsMargins(resolve(Padding::S), resolve(Padding::XXS),
                                             resolve(Padding::S), resolve(Padding::XXS));
    contentRow->layout()->setSpacing(0);

    // Vertical layout: separator on top, content below
    auto *mainCol = (new qontrol::Column)->push(separator)->push(contentRow);
    mainCol->layout()->setContentsMargins(0, 0, 0, 0);
    mainCol->layout()->setSpacing(0);

    setLayout(mainCol->layout());
}

void StatusBar::loadBlindbitUrl() {
    auto &accountOpt = m_controller->getAccount();
    if (accountOpt.has_value()) {
        auto accountName = accountOpt.value()->name();
        auto config = config_from_file(accountName);
        if (!config->is_ok()) {
            qWarning() << "StatusBar: cannot load blindbit url for account"
                       << QString::fromStdString(std::string(accountName.c_str())) << ":"
                       << QString::fromStdString(std::string(config->get_error().c_str()));
            return;
        }
        m_blindbit_url = QString::fromStdString(std::string(config->get_blindbit_url().c_str()));
    }
}

void StatusBar::loadElectrumUrl() {
    auto &accountOpt = m_controller->getAccount();
    if (accountOpt.has_value()) {
        auto accountName = accountOpt.value()->name();
        auto config = config_from_file(accountName);
        if (!config->is_ok()) {
            qWarning() << "StatusBar: cannot load electrum url for account"
                       << QString::fromStdString(std::string(accountName.c_str())) << ":"
                       << QString::fromStdString(std::string(config->get_error().c_str()));
            return;
        }
        m_electrum_url = QString::fromStdString(std::string(config->get_electrum_url().c_str()));
    }
}

void StatusBar::onUpdateConnectionState(bool connected) {
    m_connected = connected;

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

void StatusBar::onUpdateScanError(rust::String error) {
    QString errorStr = QString::fromStdString(std::string(error.c_str()));
    m_blindbit_item->setLabel(mapBackendErrorSummary(errorStr));
}

void StatusBar::onElectrumConnected(const QString &address) {
    m_electrum_connected = true;

    m_electrum_item->setOn(true);

    m_electrum_item->setLabel(TR("status-connected-electrum").arg(address));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void StatusBar::onElectrumDisconnected() {
    m_electrum_connected = false;

    m_electrum_item->setOn(false);

    m_electrum_item->setLabel(TR("status-electrum-disconnected"));
}

void StatusBar::onReloadUrl() {
    loadBlindbitUrl();
    loadElectrumUrl();
}

void StatusBar::onToggled(bool checked) {
    auto &accountOpt = m_controller->getAccount();
    if (!accountOpt.has_value()) {
        return;
    }

    if (checked) {
        m_blindbit_item->setLabel(TR("status-connecting"));
        accountOpt.value()->start_scanner();
    } else {
        m_blindbit_item->setLabel(TR("status-disconnecting"));
        accountOpt.value()->stop_scanner();
    }
}

void StatusBar::onElectrumToggled(bool checked) {
    auto &accountOpt = m_controller->getAccount();
    if (!accountOpt.has_value()) {
        return;
    }

    if (checked) {
        m_electrum_item->setLabel(TR("status-connecting"));
        accountOpt.value()->start_electrum();
    } else {
        m_electrum_item->setLabel(TR("status-disconnecting"));
        accountOpt.value()->stop_electrum();
    }
}
