#include "HwiSettings.h"

#include "HwiModule.h"
#include "HwiSigner.h"

#include "../../views/utils.h"
#include "catalog/Button.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/form/FormRow.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"

#include <QVariant>

#include <common.h>

using catalog::Button;
using catalog::ComboBox;
using catalog::FormRow;
using catalog::Input;
using catalog::Label;
using catalog::LabelRole;

namespace {
const QString K_DEFAULT_PATH = QStringLiteral("m/84'/0'/0'");
const char *const K_DEVICE_ID = "hwiDeviceId";

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
} // namespace

HwiSettings::HwiSettings(HwiModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

void HwiSettings::init() {
    m_network_selector = new ComboBox;
    m_network_selector->addItem(QStringLiteral("Bitcoin"), static_cast<int>(Network::Bitcoin));
    m_network_selector->addItem(QStringLiteral("Testnet"), static_cast<int>(Network::Testnet));
    m_network_selector->addItem(QStringLiteral("Signet"), static_cast<int>(Network::Signet));
    m_network_selector->addItem(QStringLiteral("Regtest"), static_cast<int>(Network::Regtest));
    m_network_selector->setWidth(Size::M);
    int index = m_network_selector->findData(static_cast<int>(HwiModule::network()));
    if (index >= 0) {
        m_network_selector->setCurrentIndex(index);
    }

    m_empty_label = new Label(TR("hwi-no-devices"), LabelRole::Caption);
    m_device_column = new qontrol::Column;
}

void HwiSettings::doConnect() {
    connect(m_network_selector, &QComboBox::activated, this, &HwiSettings::onNetworkChanged,
            qontrol::UNIQUE);
    connect(m_module, &HwiModule::devicesChanged, this, &HwiSettings::onDevicesChanged,
            qontrol::UNIQUE);
}

void HwiSettings::view() {
    auto *col = (new qontrol::Column)
                    ->push(new FormRow(TR("hwi-network"), m_network_selector))
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new catalog::Separator)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(new Label(TR("hwi-devices"), LabelRole::Heading))
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(m_empty_label)
                    ->push(m_device_column);

    setScreenContent(this, m_main_widget, col);
    rebuildDevices();
}

void HwiSettings::showEvent(QShowEvent *event) {
    qontrol::Screen::showEvent(event);
    // Nothing polls the bus until someone is looking at it.
    m_module->requestPolling(true);
}

void HwiSettings::hideEvent(QHideEvent *event) {
    qontrol::Screen::hideEvent(event);
    m_module->requestPolling(false);
}

void HwiSettings::onNetworkChanged() {
    auto network = static_cast<Network>(m_network_selector->currentData().toInt());
    m_module->setNetwork(network);
}

void HwiSettings::onDevicesChanged() {
    rebuildDevices();
}

void HwiSettings::rebuildDevices() {
    m_device_column->clear();
    m_rows.clear();

    auto devices = m_module->devices();
    m_empty_label->setVisible(devices.isEmpty());

    for (const auto &device : devices) {
        QString deviceId = HwiModule::toQt(device.id);
        auto *instance = m_module->deviceSigner(deviceId);
        if (instance == nullptr) {
            continue;
        }

        DeviceRow row;
        row.device_id = deviceId;
        row.signer = instance;
        row.path = new Input(K_DEFAULT_PATH);
        row.path->setWidth(Size::M);
        row.result = new Label(QString(), LabelRole::Mono);
        row.result->setWordWrap(true);

        QString detail = instance->stateDetail();
        QString summary = QStringLiteral("%1  %2  %3")
                              .arg(instance->kind(), instance->fingerprint(), instance->version())
                              .simplified();
        auto *stateLabel = new Label(detail.isEmpty() ? stateText(instance->state())
                                                      : stateText(instance->state()) + " - " + detail,
                                     LabelRole::Caption);

        auto *infoBtn = new Button(TR("hwi-info"));
        auto *xpubBtn = new Button(TR("hwi-get-xpub"));
        infoBtn->setProperty(K_DEVICE_ID, deviceId);
        xpubBtn->setProperty(K_DEVICE_ID, deviceId);
        bool ready = instance->state() == plugin::SignerState::Ready;
        infoBtn->setEnabled(ready);
        xpubBtn->setEnabled(ready);
        row.path->setEnabled(ready);

        connect(infoBtn, &QPushButton::clicked, this, &HwiSettings::onInfoClicked, qontrol::UNIQUE);
        connect(xpubBtn, &QPushButton::clicked, this, &HwiSettings::onXpubClicked, qontrol::UNIQUE);
        connect(instance, qOverload<ReqId, QMap<QString, QString>>(&ISigner::info), this,
                &HwiSettings::onSignerInfo, qontrol::UNIQUE);
        connect(instance, &ISigner::xpub, this, &HwiSettings::onSignerXpub, qontrol::UNIQUE);
        connect(instance, &ISigner::error, this, &HwiSettings::onSignerError, qontrol::UNIQUE);

        auto *actions = (new qontrol::Row)
                            ->push(infoBtn)
                            ->pushSpacer(resolve(Spacing::XS))
                            ->push(row.path)
                            ->pushSpacer(resolve(Spacing::XS))
                            ->push(xpubBtn)
                            ->pushSpacer();
        auto *content = (new qontrol::Column)
                            ->push(new Label(summary, LabelRole::Body))
                            ->push(stateLabel)
                            ->pushSpacer(resolve(Spacing::XS))
                            ->push(actions)
                            ->push(row.result);

        m_device_column->push(content)->pushSpacer(resolve(Spacing::S));
        m_rows.append(row);
    }
}

auto HwiSettings::rowFor(const HwiSigner *signer) -> DeviceRow * {
    for (auto &row : m_rows) {
        if (row.signer == signer) {
            return &row;
        }
    }
    return nullptr;
}

auto HwiSettings::rowForSender() -> DeviceRow * {
    return rowFor(qobject_cast<HwiSigner *>(sender()));
}

void HwiSettings::onInfoClicked() {
    auto *button = qobject_cast<QPushButton *>(sender());
    if (button == nullptr) {
        return;
    }
    QString deviceId = button->property(K_DEVICE_ID).toString();
    for (auto &row : m_rows) {
        if (row.device_id == deviceId && row.signer != nullptr) {
            row.pending_info = row.signer->info();
            return;
        }
    }
}

void HwiSettings::onXpubClicked() {
    auto *button = qobject_cast<QPushButton *>(sender());
    if (button == nullptr) {
        return;
    }
    QString deviceId = button->property(K_DEVICE_ID).toString();
    for (auto &row : m_rows) {
        if (row.device_id == deviceId && row.signer != nullptr) {
            row.pending_xpub = row.signer->getXpub(row.path->text().trimmed());
            row.result->setText(TR("hwi-waiting"));
            return;
        }
    }
}

void HwiSettings::onSignerInfo(ReqId req_id, const QMap<QString, QString> &info) {
    auto *row = rowForSender();
    if (row == nullptr || row->pending_info != req_id) {
        return;
    }
    row->pending_info = 0;
    row->result->setText(TR("hwi-info-template")
                             .arg(info.value(QStringLiteral("kind")),
                                  info.value(QStringLiteral("fingerprint")),
                                  info.value(QStringLiteral("version"))));
}

void HwiSettings::onSignerXpub(ReqId req_id, const QString &xpub) {
    auto *row = rowForSender();
    if (row == nullptr || row->pending_xpub != req_id) {
        return;
    }
    row->pending_xpub = 0;
    row->result->setText(xpub);
}

void HwiSettings::onSignerError(std::optional<ReqId> req_id, const QString &message) {
    auto *row = rowForSender();
    if (row == nullptr) {
        return;
    }
    if (req_id.has_value() && req_id.value() != row->pending_info &&
        req_id.value() != row->pending_xpub) {
        return;
    }
    row->pending_info = 0;
    row->pending_xpub = 0;
    row->result->setText(message);
}
