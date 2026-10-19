#include "DeviceUnlock.h"

#include "UnlockDevices.h"
#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "modules/keys/DeviceName.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QStringList>
#include <Qontrol>
#include <common.h>
#include <string>

namespace state {

using catalog::Label;
using catalog::LabelRole;

namespace {
auto toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}
} // namespace

DeviceUnlock::DeviceUnlock(const StateUnlockInfo &info, Network network)
    : m_path(toQt(info.path)),
      m_devices(new UnlockDevices(network, this)) {
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
    m_device_rows = new qontrol::Column;
    m_no_device = new catalog::ModalStatus;
    m_no_device->setIcon(icon::usb);
    m_no_device->setPadding(resolve(Spacing::M), resolve(Spacing::M));
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
        ->push(m_device_rows)
        ->push(m_no_device)
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

// Each device in sight, ready or not, so the user sees what to unlock.
void DeviceUnlock::rebuildDevices() {
    // clear() also resets the layout spacing.
    catalog::clearRows(m_device_rows);
    m_device_rows->spacing(resolve(Spacing::S));
    auto devices = m_devices->devices();
    for (const auto &device : devices) {
        bool ready = device.state == HwiDeviceState::Supported;
        auto *row = new catalog::SelectRow;
        row->setIcon(icon::usb);
        row->setTitle(keys::deviceName(toQt(device.kind), QString()));
        row->setSub(ready ? toQt(device.fingerprint) : toQt(device.detail));
        if (ready) {
            row->setStatus(catalog::pillDeviceReady());
        } else if (device.state == HwiDeviceState::Locked) {
            row->setStatus(catalog::pillDeviceLocked());
        } else {
            row->setStatus(catalog::pillDeviceUnsupported());
        }
        row->setChevron(false);
        row->setClickable(false);
        m_device_rows->push(row);
        row->show();
    }
    m_device_rows->setVisible(!devices.isEmpty());
    m_no_device->setVisible(devices.isEmpty());
    adjustSize();
}

void DeviceUnlock::askAll() {
    for (const auto &device : m_devices->devices()) {
        QString id = toQt(device.id);
        if (device.state != HwiDeviceState::Supported || m_asked.contains(id)) {
            continue;
        }
        m_asked.insert(id);
        quint64 reqId = m_devices->fetchXpub(id, m_path);
        if (reqId != 0) {
            m_pending.insert(reqId, keys::deviceName(toQt(device.kind), QString()));
        }
    }
    if (!m_pending.isEmpty()) {
        showBusy();
    } else if (m_view == View::Busy) {
        showWaiting();
    }
}

void DeviceUnlock::settle(const QString &error) {
    if (!m_pending.isEmpty()) {
        showBusy();
        return;
    }
    m_error->setText(error);
    m_error->setVisible(true);
    m_try_again->setVisible(true);
    showWaiting();
}

// ===== Busy view =====

void DeviceUnlock::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

void DeviceUnlock::showBusy() {
    m_view = View::Busy;
    QStringList names = m_pending.values();
    names.sort();
    m_busy->setTitle(TR("state-device-confirm"));
    m_busy->setSubtitle(
        TR("state-device-confirm-sub").arg(names.join(QStringLiteral(", ")), m_path));
    setHeader(nullptr);
    setBody(m_busy);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

// ===== Slots =====

void DeviceUnlock::onDevicesChanged() {
    rebuildDevices();
    // A device seen for the first time is asked, Try Again shown or not.
    askAll();
}

void DeviceUnlock::onXpubFetched(quint64 req_id, const QString &xpub) {
    if (!m_pending.contains(req_id)) {
        return;
    }
    m_pending.remove(req_id);
    QString error = toQt(::state_unlock_with_device(rust::String(xpub.toStdString())));
    if (error.isEmpty()) {
        accept();
        return;
    }
    settle(TR("state-unlock-wrong-key"));
}

void DeviceUnlock::onXpubFailed(quint64 req_id, const QString &error) {
    if (!m_pending.contains(req_id)) {
        return;
    }
    m_pending.remove(req_id);
    settle(TR("state-device-failed").arg(error));
}

void DeviceUnlock::onTryAgainClicked() {
    m_asked.clear();
    m_error->setVisible(false);
    m_try_again->setVisible(false);
    askAll();
}

void DeviceUnlock::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void DeviceUnlock::retranslateUi() {
    m_text->setText(TR("state-unlock-device-text"));
    m_no_device->setTitle(TR("state-no-device"));
    m_no_device->setSubtitle(TR("state-no-device-sub"));
    m_try_again->setText(TR("btn-try-again"));
}

} // namespace state
