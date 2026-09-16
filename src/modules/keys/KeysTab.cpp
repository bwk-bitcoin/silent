#include "KeysTab.h"

#include "KeysModule.h"

#include "modules/hwi/HwiModule.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/form/FormRow.h"
#include "catalog/inputs/ComboBox.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <QLabel>
#include <QStringList>
#include <QVariant>

#include <common.h>

using catalog::Badge;
using catalog::Button;
using catalog::ButtonRole;
using catalog::ComboBox;
using catalog::FormRow;
using catalog::Label;
using catalog::LabelRole;

namespace keys {

namespace {
const char *const K_FINGERPRINT = "keyFingerprint";
const char *const K_ALIAS = "keyAlias";

auto stateText(plugin::SignerState state) -> QString {
    switch (state) {
    case plugin::SignerState::Ready:
        return TR("hwi-state-ready");
    case plugin::SignerState::Locked:
        return TR("hwi-state-locked");
    default:
        return TR("hwi-state-unsupported");
    }
}

auto routeIcon(KeyRouteKind kind) -> QIcon {
    switch (kind) {
    case KeyRouteKind::Hot:
        return icon::key(IconColor::Accent);
    case KeyRouteKind::Usb:
        return icon::usb(IconColor::Accent);
    case KeyRouteKind::Qr:
        return icon::qr(IconColor::Accent);
    case KeyRouteKind::SdCard:
        return icon::sdCard(IconColor::Accent);
    default:
        return icon::key(IconColor::Muted);
    }
}

auto routeTooltip(KeyRouteKind kind) -> QString {
    switch (kind) {
    case KeyRouteKind::Hot:
        return TR("keys-route-hot");
    case KeyRouteKind::Usb:
        return TR("keys-route-usb");
    case KeyRouteKind::Qr:
        return TR("keys-route-qr");
    case KeyRouteKind::SdCard:
        return TR("keys-route-sd");
    default:
        return QString();
    }
}
} // namespace

KeysTab::KeysTab(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

void KeysTab::init() {
    m_network_selector = new ComboBox;
    m_network_selector->addItem(TR("network-regtest"), static_cast<int>(Network::Regtest));
    m_network_selector->addItem(TR("network-signet"), static_cast<int>(Network::Signet));
    m_network_selector->addItem(TR("network-testnet"), static_cast<int>(Network::Testnet));
    m_network_selector->addItem(TR("network-bitcoin"), static_cast<int>(Network::Bitcoin));
    m_network_selector->setWidth(Size::M);
    int index = m_network_selector->findData(static_cast<int>(KeysModule::network()));
    if (index >= 0) {
        m_network_selector->setCurrentIndex(index);
    }

    m_devices_empty = new Label(TR("keys-no-devices"), LabelRole::Caption);
    m_error_label = new Label(QString(), LabelRole::Caption);
    m_error_label->setVisible(false);
    m_device_column = new qontrol::Column;

    m_keys_empty = new Label(TR("keys-no-keys"), LabelRole::Caption);
    m_key_column = new qontrol::Column;
    m_add_key_btn = new Button(TR("keys-add-key"), ButtonRole::Primary);
    m_add_key_btn->setIcon(icon::plus(IconColor::Accent));
}

void KeysTab::doConnect() {
    connect(m_network_selector, &QComboBox::activated, this, &KeysTab::onNetworkChanged,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::keysChanged, this, &KeysTab::onKeysChanged, qontrol::UNIQUE);
    connect(m_module, &KeysModule::liveDevicesChanged, this, &KeysTab::onDevicesChanged,
            qontrol::UNIQUE);
}

void KeysTab::view() {
    auto *col = (new qontrol::Column)
                    ->push(new FormRow(TR("keys-network"), m_network_selector))
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::Separator)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new Label(TR("keys-devices"), LabelRole::Heading))
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(m_error_label)
                    ->push(m_devices_empty)
                    ->push(m_device_column)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::Separator)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new Label(TR("keys-keys"), LabelRole::Heading))
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(m_keys_empty)
                    ->push(m_key_column)
                    ->pushSpacer(resolve(Spacing::S))
                    // The add key modal that wires m_add_key_btn's clicked signal is added
                    // by the task that creates it.
                    ->push((new qontrol::Row)->push(m_add_key_btn)->pushSpacer())
                    ->pushSpacer();

    setScreenContent(this, m_main_widget, col);
    rebuildDevices();
    rebuildKeys();
}

void KeysTab::showEvent(QShowEvent *event) {
    qontrol::Screen::showEvent(event);
    // Nothing polls the bus until someone is looking at it.
    Host::get()->requestSignerPolling(m_module, true);
}

void KeysTab::hideEvent(QHideEvent *event) {
    qontrol::Screen::hideEvent(event);
    Host::get()->requestSignerPolling(m_module, false);
}

void KeysTab::onNetworkChanged() {
    auto network = static_cast<Network>(m_network_selector->currentData().toInt());
    // The hwi module owns the persistence, the keys module owns the records, and one
    // combobox drives both.
    m_module->setNetwork(network);
    if (m_module->hwi() != nullptr) {
        m_module->hwi()->setNetwork(network);
    }
    rebuildDevices();
}

void KeysTab::onDevicesChanged() {
    rebuildDevices();
    rebuildKeys();
}

void KeysTab::onKeysChanged() {
    rebuildDevices();
    rebuildKeys();
}

void KeysTab::rebuildDevices() {
    m_device_column->clear();

    bool hasDevice = false;
    for (const auto &entry : Host::get()->signers()) {
        auto *signer = entry.signer;
        if (signer == nullptr) {
            continue;
        }
        hasDevice = true;

        QString fingerprint = signer->fingerprint();
        QString title = QStringLiteral("%1 %2").arg(signer->brand(), signer->model()).simplified();
        if (title.isEmpty()) {
            title = signer->id();
        }

        QStringList parts;
        if (!fingerprint.isEmpty()) {
            parts << fingerprint;
        }
        parts << stateText(signer->state());
        if (!signer->stateDetail().isEmpty()) {
            parts << signer->stateDetail();
        }

        auto *row = new catalog::SelectRow;
        row->setIcon(icon::usb(IconColor::Accent));
        row->setTitle(title);
        row->setMetadata(parts.join(QStringLiteral("  ")));

        auto *deviceRow = (new qontrol::Row)->push(row)->pushSpacer();

        if (!fingerprint.isEmpty() && !m_module->hasKey(fingerprint)) {
            auto *addBtn = new Button(ButtonRole::InlineIcon);
            addBtn->setIcon(icon::plus(IconColor::Accent));
            addBtn->setToolTip(TR("keys-register"));
            addBtn->setProperty(K_FINGERPRINT, fingerprint);
            addBtn->setProperty(K_ALIAS, title);
            connect(addBtn, &QPushButton::clicked, this, &KeysTab::onRegisterDeviceClicked,
                    qontrol::UNIQUE);
            deviceRow->push(addBtn);
        }

        m_device_column->push(deviceRow)->pushSpacer(resolve(Spacing::S));
    }

    m_devices_empty->setVisible(!hasDevice);
}

void KeysTab::rebuildKeys() {
    m_key_column->clear();

    if (m_module->rust() == nullptr) {
        m_keys_empty->setVisible(true);
        return;
    }

    QList<KeyInfo> keyList = m_module->keys();
    m_keys_empty->setVisible(keyList.isEmpty());

    for (const auto &keyInfo : keyList) {
        QString fingerprint = KeysModule::toQt(keyInfo.fingerprint);
        QString alias = KeysModule::toQt(keyInfo.alias);
        QString title = alias.isEmpty() ? fingerprint : alias;

        // The key modal that wires this row's clicked signal is added by the task that
        // creates it.
        auto *row = new catalog::SelectRow;
        row->setIcon(icon::key(IconColor::Accent));
        row->setTitle(title);
        row->setMetadata(fingerprint);
        row->setProperty(K_FINGERPRINT, fingerprint);

        auto *routeStrip = new qontrol::Row;
        for (const auto &routeKind : keyInfo.routes) {
            auto *routeIconLabel = new QLabel;
            routeIconLabel->setPixmap(routeIcon(routeKind).pixmap(metric::SELECT_ROW_ICON_SIZE,
                                                                  metric::SELECT_ROW_ICON_SIZE));
            routeIconLabel->setToolTip(routeTooltip(routeKind));
            routeStrip->push(routeIconLabel);
        }

        Badge *statusBadge = nullptr;
        if (keyInfo.is_connected) {
            statusBadge = new Badge(TR("keys-connected"), Badge::Role::Success);
        } else {
            statusBadge = new Badge(TR("keys-offline"), Badge::Role::Neutral);
        }

        auto *keyRow =
            (new qontrol::Row)->push(row)->pushSpacer()->push(routeStrip)->push(statusBadge);
        m_key_column->push(keyRow)->pushSpacer(resolve(Spacing::S));
    }
}

void KeysTab::onRegisterDeviceClicked() {
    auto *button = qobject_cast<QPushButton *>(sender());
    if (button == nullptr) {
        return;
    }
    QString fingerprint = button->property(K_FINGERPRINT).toString();
    QString alias = button->property(K_ALIAS).toString();

    if (m_module->rust() == nullptr) {
        return;
    }

    auto result = m_module->rust()->add_key(rust::String(fingerprint.toStdString()),
                                            rust::String(alias.toStdString()));
    if (!result.is_ok) {
        showError(TR("keys-register-failed").arg(KeysModule::toQt(result.error)));
        return;
    }

    m_error_label->setVisible(false);
    rebuildDevices();
}

void KeysTab::showError(const QString &message) {
    m_error_label->setText(message);
    m_error_label->setVisible(!message.isEmpty());
}

} // namespace keys
