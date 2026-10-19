#include "StateSetup.h"

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
#include "catalog/form/Field.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysView.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QLineEdit>
#include <QMetaObject>
#include <QThread>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <string>

namespace state {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {
constexpr int MIN_PASSWORD_LENGTH = 8;
constexpr const char *K_DEVICE_ID = "deviceId";

auto toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}
} // namespace

StateSetup::StateSetup() {
    init();
    doConnect();
    view();
}

StateSetup::~StateSetup() {
    m_password_input->clear();
    m_confirm_input->clear();
}

void StateSetup::reject() {
    if (m_sealing) {
        return;
    }
    catalog::Modal::reject();
}

void StateSetup::init() {
    initChoice();
    initDevice();
    initPassword();
    initBusy();
}

void StateSetup::doConnect() {
    connect(m_device_choice, &catalog::SelectRow::clicked, this, &StateSetup::onDeviceChosen,
            qontrol::UNIQUE);
    connect(m_password_choice, &catalog::SelectRow::clicked, this, &StateSetup::onPasswordChosen,
            qontrol::UNIQUE);
    connect(m_network, &QComboBox::activated, this, &StateSetup::onNetworkActivated,
            qontrol::UNIQUE);
    connect(m_device_continue, &QPushButton::clicked, this, &StateSetup::onDeviceContinueClicked,
            qontrol::UNIQUE);
    connect(m_device_actions, &catalog::ModalActions::backClicked, this, &StateSetup::onBackClicked,
            qontrol::UNIQUE);
    for (auto *input : {m_password_input, m_confirm_input}) {
        connect(input, &QLineEdit::textEdited, this, &StateSetup::onPasswordEdited,
                qontrol::UNIQUE);
    }
    connect(m_confirm_input, &QLineEdit::returnPressed, this, &StateSetup::onCreatePasswordClicked,
            qontrol::UNIQUE);
    connect(m_create, &QPushButton::clicked, this, &StateSetup::onCreatePasswordClicked,
            qontrol::UNIQUE);
    connect(m_password_actions, &catalog::ModalActions::backClicked, this,
            &StateSetup::onBackClicked, qontrol::UNIQUE);
}

void StateSetup::view() {
    m_choice = viewChoice();
    m_device = viewDevice();
    m_password = viewPassword();
    keep(m_device);
    keep(m_device_actions);
    keep(m_password);
    keep(m_password_actions);
    keep(m_busy);
    retranslateUi();
    showChoice();
}

// ===== Choice view =====

void StateSetup::initChoice() {
    m_choice_text = new Label(LabelRole::Body);
    m_choice_text->setWordWrap(true);
    m_device_choice = new catalog::SelectRow;
    m_device_choice->setIcon(icon::usb);
    m_password_choice = new catalog::SelectRow;
    m_password_choice->setIcon(icon::lock);
}

auto StateSetup::viewChoice() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_choice_text)
        ->push((new qontrol::Column)
                   ->spacing(resolve(Spacing::S))
                   ->push(m_device_choice)
                   ->push(m_password_choice));
}

void StateSetup::showChoice() {
    m_view = View::Choice;
    setWindowTitle(TR("state-setup-title"));
    setHeader(nullptr);
    setBody(m_choice);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

// ===== Device view =====

void StateSetup::initDevice() {
    m_device_text = new Label(LabelRole::Body);
    m_device_text->setTone(LabelTone::Secondary);
    m_device_text->setWordWrap(true);
    m_network = new catalog::ComboBox;
    for (auto network : {Network::Bitcoin, Network::Testnet, Network::Signet, Network::Regtest}) {
        m_network->addItem(keys::networkLabel(network), static_cast<int>(network));
    }
    m_network->setWidth(Size::S);
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
}

auto StateSetup::viewDevice() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_device_text)
        ->push(new catalog::Field(TR("keys-network"), m_network))
        ->push(m_device_rows)
        ->push(m_no_device)
        ->push(m_device_error);
}

void StateSetup::showDevice() {
    m_view = View::Device;
    if (m_devices == nullptr) {
        auto network = static_cast<Network>(m_network->currentData().toInt());
        m_devices = new UnlockDevices(network, this);
        connect(m_devices, &UnlockDevices::devicesChanged, this, &StateSetup::onDevicesChanged,
                qontrol::UNIQUE);
        connect(m_devices, &UnlockDevices::xpubFetched, this, &StateSetup::onXpubFetched,
                qontrol::UNIQUE);
        connect(m_devices, &UnlockDevices::xpubFailed, this, &StateSetup::onXpubFailed,
                qontrol::UNIQUE);
    }
    rebuildDevices();
    setWindowTitle(TR("state-device-title"));
    setHeader(nullptr);
    setBody(m_device);
    setFooter(m_device_actions);
    setWidth(ModalWidth::M);
}

void StateSetup::rebuildDevices() {
    catalog::clearRows(m_device_rows);
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
        connect(row, &catalog::SelectRow::clicked, this, &StateSetup::onDeviceRowClicked,
                qontrol::UNIQUE);
        m_device_rows->push(row);
        row->show();
        if (ready && id == m_selected_id) {
            selectedSeen = true;
            m_selected_fingerprint = toQt(device.fingerprint);
            m_selected_name = keys::deviceName(toQt(device.kind), QString());
        }
    }
    if (!selectedSeen) {
        m_selected_id.clear();
    }
    m_device_rows->setVisible(!devices.isEmpty());
    m_no_device->setVisible(devices.isEmpty());
    updateDeviceActions();
    adjustSize();
}

void StateSetup::updateDeviceActions() {
    m_device_continue->setEnabled(!m_selected_id.isEmpty());
    m_device_actions->setReason(m_selected_id.isEmpty() ? TR("state-device-pick") : QString());
}

// ===== Password view =====

void StateSetup::initPassword() {
    m_password_text = new Label(LabelRole::Body);
    m_password_text->setTone(LabelTone::Secondary);
    m_password_text->setWordWrap(true);
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_confirm_input = new catalog::Input;
    m_confirm_input->setEchoMode(QLineEdit::Password);
    m_confirm_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_short =
        new catalog::FieldNote(TR("keys-password-short"), catalog::FieldNote::Tone::Error);
    m_password_mismatch =
        new catalog::FieldNote(TR("keys-password-no-match"), catalog::FieldNote::Tone::Error);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_password_actions = new catalog::ModalActions;
    m_password_actions->setBack(catalog::ModalActions::Back::Back);
    m_create = catalog::btnCreate();
    m_password_actions->addAction(m_create);
}

auto StateSetup::viewPassword() -> QWidget * {
    m_password_fields = new catalog::FieldGroup;
    m_password_fields->addField(TR("keys-password-field"), m_password_input)
        ->addNote(m_password_short)
        ->addField(TR("keys-password-confirm-field"), m_confirm_input)
        ->addNote(m_password_mismatch);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_password_text)
        ->push(m_password_fields)
        ->push(m_password_error);
}

void StateSetup::showPassword() {
    m_view = View::Password;
    onPasswordEdited();
    setWindowTitle(TR("state-password-title"));
    setHeader(nullptr);
    setBody(m_password);
    setFooter(m_password_actions);
    setWidth(ModalWidth::M);
    m_password_input->setFocus();
}

// ===== Busy view =====

void StateSetup::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

void StateSetup::showBusy(const QString &title, const QString &subtitle) {
    m_view = View::Busy;
    m_busy->setTitle(title);
    m_busy->setSubtitle(subtitle);
    setHeader(nullptr);
    setBody(m_busy);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

// ===== Slots =====

void StateSetup::onDeviceChosen() {
    showDevice();
}

void StateSetup::onPasswordChosen() {
    showPassword();
}

void StateSetup::onBackClicked() {
    m_device_error->setVisible(false);
    m_password_error->setVisible(false);
    showChoice();
}

void StateSetup::onNetworkActivated(int index) {
    Q_UNUSED(index);
    // A device's app is network specific: look again with the network picked.
    delete m_devices;
    m_devices = nullptr;
    m_selected_id.clear();
    showDevice();
}

void StateSetup::onDevicesChanged() {
    if (m_view == View::Device) {
        rebuildDevices();
    }
}

void StateSetup::onDeviceRowClicked() {
    if (sender() == nullptr) {
        return;
    }
    m_selected_id = sender()->property(K_DEVICE_ID).toString();
    m_device_error->setVisible(false);
    rebuildDevices();
}

void StateSetup::onDeviceContinueClicked() {
    if (m_selected_id.isEmpty() || m_devices == nullptr) {
        return;
    }
    m_index = ::state_draw_index();
    QString path = toQt(::state_device_path(m_index));
    m_xpub_req = m_devices->fetchXpub(m_selected_id, path);
    if (m_xpub_req == 0) {
        m_device_error->setText(TR("state-device-unreachable"));
        m_device_error->setVisible(true);
        return;
    }
    showBusy(TR("state-device-confirm"), TR("state-device-confirm-sub").arg(m_selected_name, path));
}

void StateSetup::onXpubFetched(quint64 req_id, const QString &xpub) {
    if (req_id != m_xpub_req) {
        return;
    }
    m_xpub_req = 0;
    auto network = static_cast<Network>(m_network->currentData().toInt());
    QString error =
        toQt(::state_create_with_device(rust::String(m_selected_fingerprint.toStdString()),
                                        rust::String(m_selected_name.toStdString()), m_index,
                                        network, rust::String(xpub.toStdString())));
    if (error.isEmpty()) {
        accept();
        return;
    }
    m_device_error->setText(error);
    m_device_error->setVisible(true);
    showDevice();
}

void StateSetup::onXpubFailed(quint64 req_id, const QString &error) {
    if (req_id != m_xpub_req) {
        return;
    }
    m_xpub_req = 0;
    m_device_error->setText(TR("state-device-failed").arg(error));
    m_device_error->setVisible(true);
    showDevice();
}

void StateSetup::onPasswordEdited() {
    QString password = m_password_input->text();
    bool tooShort = !password.isEmpty() && password.size() < MIN_PASSWORD_LENGTH;
    bool mismatch = !m_confirm_input->text().isEmpty() && m_confirm_input->text() != password;
    m_password_short->setVisible(tooShort);
    m_password_mismatch->setVisible(mismatch);
    QString reason;
    if (password.isEmpty()) {
        reason = TR("keys-password-choose");
    } else if (password.size() < MIN_PASSWORD_LENGTH) {
        reason = TR("keys-password-short");
    } else if (m_confirm_input->text() != password) {
        reason = TR("keys-password-must-match");
    }
    m_create->setEnabled(reason.isEmpty());
    m_password_actions->setReason(reason);
}

void StateSetup::onCreatePasswordClicked() {
    if (m_view != View::Password || !m_create->isEnabled()) {
        return;
    }
    std::string password = m_password_input->text().toStdString();
    m_password_input->clear();
    m_confirm_input->clear();
    m_password_error->setVisible(false);
    showBusy(TR("state-password-sealing"), TR("state-password-sealing-sub"));
    m_sealing = true;
    auto *worker = QThread::create([this, password]() mutable {
        QString error = toQt(::state_create_with_password(rust::String(password)));
        std::ranges::fill(password, '\0');
        QMetaObject::invokeMethod(this, "onPasswordCreated", Qt::QueuedConnection,
                                  Q_ARG(QString, error));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void StateSetup::onPasswordCreated(const QString &error) {
    m_sealing = false;
    if (error.isEmpty()) {
        accept();
        return;
    }
    m_password_error->setText(error);
    m_password_error->setVisible(true);
    showPassword();
}

void StateSetup::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void StateSetup::retranslateUi() {
    m_choice_text->setText(TR("state-setup-text"));
    m_device_choice->setTitle(TR("state-choice-device"));
    m_device_choice->setSub(TR("state-choice-device-sub"));
    m_password_choice->setTitle(TR("state-choice-password"));
    m_password_choice->setSub(TR("state-choice-password-sub"));
    m_device_text->setText(TR("state-device-text"));
    m_no_device->setTitle(TR("state-no-device"));
    m_no_device->setSubtitle(TR("state-no-device-sub"));
    m_password_text->setText(TR("state-password-text"));
    m_device_continue->setText(TR("btn-continue"));
    m_create->setText(TR("btn-create"));
}

} // namespace state
