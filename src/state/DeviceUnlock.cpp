#include "DeviceUnlock.h"

#include "UnlockDevices.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>
#include <optional>
#include <string>

namespace state {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {
auto toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}
} // namespace

DeviceUnlock::DeviceUnlock(const StateUnlockInfo &info)
    : m_fingerprint(toQt(info.fingerprint)),
      m_alias(toQt(info.alias)),
      m_path(toQt(::state_device_path(info.index))),
      m_devices(new UnlockDevices(info.network, this)) {
    init();
    doConnect();
    view();
}

void DeviceUnlock::init() {
    initWaiting();
    initBusy();
}

void DeviceUnlock::doConnect() {
    connect(m_devices, &UnlockDevices::devicesChanged, this, &DeviceUnlock::onDevicesChanged,
            qontrol::UNIQUE);
    connect(m_devices, &UnlockDevices::xpubFetched, this, &DeviceUnlock::onXpubFetched,
            qontrol::UNIQUE);
    connect(m_devices, &UnlockDevices::xpubFailed, this, &DeviceUnlock::onXpubFailed,
            qontrol::UNIQUE);
    connect(m_try_again, &QPushButton::clicked, this, &DeviceUnlock::onTryAgainClicked,
            qontrol::UNIQUE);
}

void DeviceUnlock::view() {
    m_waiting = viewWaiting();
    keep(m_busy);
    retranslateUi();
    showWaiting();
    onDevicesChanged();
}

// ===== Waiting view =====

void DeviceUnlock::initWaiting() {
    m_text = new Label(LabelRole::Body);
    m_text->setWordWrap(true);
    m_device = new catalog::SelectRow;
    m_device->setIcon(icon::usb);
    m_device->setTitle(m_alias);
    m_device->setMeta(m_fingerprint);
    m_device->setChevron(false);
    m_device->setClickable(false);
    m_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_error->setVisible(false);
    m_actions = new catalog::ModalActions;
    m_try_again = catalog::btnTryAgain();
    m_try_again->setVisible(false);
    m_actions->addAction(m_try_again);
}

auto DeviceUnlock::viewWaiting() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_text)
        ->push(m_device)
        ->push(m_error);
}

void DeviceUnlock::showWaiting() {
    m_view = View::Waiting;
    setWindowTitle(TR("state-unlock-title"));
    setHeader(nullptr);
    setBody(m_waiting);
    setFooter(m_actions);
    setWidth(ModalWidth::M);
}

// The device's row: not connected, locked or ready, and its request sent once ready.
void DeviceUnlock::updateDevice() {
    std::optional<HwiDevice> found;
    bool seenLocked = false;
    for (const auto &device : m_devices->devices()) {
        if (device.state == HwiDeviceState::Supported &&
            toQt(device.fingerprint) == m_fingerprint) {
            found = device;
        } else if (device.state == HwiDeviceState::Locked) {
            seenLocked = true;
        }
    }
    if (found.has_value()) {
        m_device->setStatus(catalog::pillDeviceReady());
        m_device->setSub(TR("state-unlock-ready"));
    } else if (seenLocked) {
        m_device->setStatus(catalog::pillDeviceLocked());
        m_device->setSub(TR("state-unlock-locked"));
    } else {
        m_device->setStatus(catalog::pillSignerDisconnected());
        m_device->setSub(TR("state-unlock-plug"));
    }
    if (!found.has_value() || m_held || m_xpub_req != 0) {
        return;
    }
    m_xpub_req = m_devices->fetchXpub(toQt(found->id), m_path);
    if (m_xpub_req != 0) {
        showBusy();
    }
}

// ===== Busy view =====

void DeviceUnlock::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

void DeviceUnlock::showBusy() {
    m_view = View::Busy;
    m_busy->setTitle(TR("state-device-confirm"));
    m_busy->setSubtitle(TR("state-device-confirm-sub").arg(m_alias, m_path));
    setHeader(nullptr);
    setBody(m_busy);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

// ===== Slots =====

void DeviceUnlock::onDevicesChanged() {
    updateDevice();
}

void DeviceUnlock::onXpubFetched(quint64 req_id, const QString &xpub) {
    if (req_id != m_xpub_req) {
        return;
    }
    m_xpub_req = 0;
    QString error = toQt(::state_unlock_with_device(rust::String(xpub.toStdString())));
    if (error.isEmpty()) {
        accept();
        return;
    }
    m_held = true;
    m_error->setText(TR("state-unlock-wrong-key"));
    m_error->setVisible(true);
    m_try_again->setVisible(true);
    showWaiting();
    updateDevice();
}

void DeviceUnlock::onXpubFailed(quint64 req_id, const QString &error) {
    if (req_id != m_xpub_req) {
        return;
    }
    m_xpub_req = 0;
    m_held = true;
    m_error->setText(TR("state-device-failed").arg(error));
    m_error->setVisible(true);
    m_try_again->setVisible(true);
    showWaiting();
    updateDevice();
}

void DeviceUnlock::onTryAgainClicked() {
    m_held = false;
    m_error->setVisible(false);
    m_try_again->setVisible(false);
    updateDevice();
}

void DeviceUnlock::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void DeviceUnlock::retranslateUi() {
    m_text->setText(TR("state-unlock-device-text").arg(m_alias, m_fingerprint));
    m_try_again->setText(TR("btn-try-again"));
}

} // namespace state
