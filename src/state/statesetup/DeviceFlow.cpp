#include "DeviceFlow.h"

#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "modules/keys/DeviceName.h"
#include "state/UnlockDevices.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QEvent>
#include <QThread>
#include <Qontrol>
#include <common.h>
#include <string>

namespace state {

namespace {
constexpr const char *K_DEVICE_ID = "deviceId";

auto toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}
} // namespace

DeviceFlow::DeviceFlow(Network network) : m_network(network) {
    init();
    doConnect();
    view();
}

void DeviceFlow::start() {
    showDevice();
}

void DeviceFlow::init() {
    initDevice();
    initBusy();
}

void DeviceFlow::doConnect() {
    connectDevice();
}

void DeviceFlow::view() {
    m_device = viewDevice();
    m_busy = viewBusy();
    hideDevice();
    hideBusy();
    retranslateUi();
}

// ===== Device view =====

void DeviceFlow::initDevice() {
    m_device_text = new catalog::Label(catalog::LabelRole::Body);
    m_device_text->setTone(catalog::LabelTone::Secondary);
    m_device_text->setWordWrap(true);
    m_device_rows = new qontrol::Column;
    m_device_rows->spacing(resolve(Spacing::S));
    m_no_device = new catalog::ModalStatus;
    m_no_device->setIcon(icon::usb);
    m_no_device->setPadding(resolve(Spacing::M), resolve(Spacing::M));
    m_device_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_device_error->setVisible(false);
    m_device_actions = new catalog::ModalActions;
    m_device_actions->setBack(catalog::ModalActions::Back::Back);
    m_device_continue = catalog::btnContinue();
    m_device_actions->addAction(m_device_continue);
    m_devices = new UnlockDevices(m_network, this);
}

void DeviceFlow::connectDevice() {
    connect(m_device_continue, &QPushButton::clicked, this, &DeviceFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_device_actions, &catalog::ModalActions::backClicked, this, &DeviceFlow::onBackClicked,
            qontrol::UNIQUE);
    connect(m_devices, &UnlockDevices::devicesChanged, this, &DeviceFlow::onDevicesChanged,
            qontrol::UNIQUE);
    connect(m_devices, &UnlockDevices::xpubFetched, this, &DeviceFlow::onXpubFetched,
            qontrol::UNIQUE);
    connect(m_devices, &UnlockDevices::xpubFailed, this, &DeviceFlow::onXpubFailed,
            qontrol::UNIQUE);
}

auto DeviceFlow::viewDevice() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_device_text)
        ->push(m_device_rows)
        ->push(m_no_device)
        ->push(m_device_error);
}

void DeviceFlow::showDevice() {
    m_view = View::Device;
    rebuildDevices();
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_device);
    viewSlots()->setFooter(m_device_actions);
    emit viewChanged(TR("state-device-title"), ModalWidth::M);
}

void DeviceFlow::hideDevice() {
    viewSlots()->keep(m_device);
    viewSlots()->keep(m_device_actions);
}

void DeviceFlow::rebuildDevices() {
    // clear() also resets the layout spacing.
    catalog::clearRows(m_device_rows);
    m_device_rows->spacing(resolve(Spacing::S));
    QList<HwiDevice> devices = m_devices == nullptr ? QList<HwiDevice>() : m_devices->devices();
    bool selectedSeen = false;
    for (const auto &device : devices) {
        QString id = toQt(device.id);
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
        row->setClickable(ready);
        row->setSelected(ready && id == m_selected_id);
        row->setProperty(K_DEVICE_ID, id);
        connect(row, &catalog::SelectRow::clicked, this, &DeviceFlow::onDeviceRowClicked,
                qontrol::UNIQUE);
        m_device_rows->push(row);
        row->show();
        if (ready && id == m_selected_id) {
            selectedSeen = true;
            m_selected_name = keys::deviceName(toQt(device.kind), QString());
        }
    }
    if (!selectedSeen) {
        m_selected_id.clear();
    }
    m_device_rows->setVisible(!devices.isEmpty());
    m_no_device->setVisible(devices.isEmpty());
    updateDeviceActions();
    updateGeometry();
}

void DeviceFlow::updateDeviceActions() {
    m_device_continue->setEnabled(!m_selected_id.isEmpty());
    m_device_actions->setReason(m_selected_id.isEmpty() ? TR("state-device-pick") : QString());
}

void DeviceFlow::fetchXpub() {
    if (m_selected_id.isEmpty() || m_devices == nullptr) {
        return;
    }
    auto drawn = ::state_draw_index();
    if (!drawn.is_ok) {
        m_device_error->setText(toQt(drawn.error));
        m_device_error->setVisible(true);
        return;
    }
    m_index = drawn.index;
    QString path = toQt(::state_device_path(m_index));
    m_xpub_req = m_devices->fetchXpub(m_selected_id, path);
    if (m_xpub_req == 0) {
        m_device_error->setText(TR("state-device-unreachable"));
        m_device_error->setVisible(true);
        return;
    }
    showBusy(path);
}

// ===== Busy view =====

void DeviceFlow::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

auto DeviceFlow::viewBusy() -> catalog::ModalStatus * {
    return m_busy;
}

void DeviceFlow::showBusy(const QString &path) {
    m_view = View::Busy;
    // A displayed device request and the durable key commit cannot be cancelled here.
    emit busyChanged(true);
    m_busy->setTitle(TR("state-device-confirm"));
    m_busy->setSubtitle(TR("state-device-confirm-sub").arg(m_selected_name, path));
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_busy);
    viewSlots()->setFooter(nullptr);
    emit viewChanged(TR("state-device-title"), ModalWidth::M);
}

void DeviceFlow::hideBusy() {
    viewSlots()->keep(m_busy);
}

// ===== Shared =====

void DeviceFlow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void DeviceFlow::retranslateUi() {
    m_device_text->setText(TR("state-device-text"));
    m_no_device->setTitle(TR("state-no-device"));
    m_no_device->setSubtitle(TR("state-no-device-sub"));
    m_device_continue->setText(TR("btn-continue"));
}

// ===== Slots =====

void DeviceFlow::onBackClicked() {
    switch (m_view) {
    case View::Device:
        delete m_devices;
        m_devices = nullptr;
        emit back();
        break;
    case View::Busy:
        break;
    }
}

void DeviceFlow::onNextClicked() {
    switch (m_view) {
    case View::Device:
        fetchXpub();
        break;
    case View::Busy:
        break;
    }
}

void DeviceFlow::onDevicesChanged() {
    if (m_view == View::Device) {
        rebuildDevices();
    }
}

void DeviceFlow::onDeviceRowClicked() {
    if (sender() == nullptr) {
        return;
    }
    m_selected_id = sender()->property(K_DEVICE_ID).toString();
    m_device_error->setVisible(false);
    rebuildDevices();
}

void DeviceFlow::onXpubFetched(quint64 req_id, const QString &xpub) {
    if (m_xpub_req == 0 || req_id != m_xpub_req) {
        return;
    }
    m_xpub_req = 0;
    m_result = std::make_shared<QString>();
    auto *worker = QThread::create([result = m_result, index = m_index, xpub]() {
        *result = toQt(::state_create_with_device(index, xpub.toStdString()));
    });
    connect(worker, &QThread::finished, this, &DeviceFlow::onDeviceCreated,
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(worker, &QThread::finished, worker, &QObject::deleteLater, qontrol::UNIQUE);
    worker->start();
}

void DeviceFlow::onXpubFailed(quint64 req_id, const QString &error) {
    if (m_xpub_req == 0 || req_id != m_xpub_req) {
        return;
    }
    m_xpub_req = 0;
    emit busyChanged(false);
    m_device_error->setText(TR("state-device-failed").arg(error));
    m_device_error->setVisible(true);
    showDevice();
}

void DeviceFlow::onDeviceCreated() {
    QString error = *m_result;
    m_result.reset();
    emit busyChanged(false);
    if (error.isEmpty()) {
        emit done();
        return;
    }
    m_device_error->setText(error);
    m_device_error->setVisible(true);
    showDevice();
}

} // namespace state
