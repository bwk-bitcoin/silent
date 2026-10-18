#include "KeysTab.h"

#include "DeviceName.h"
#include "KeysModule.h"
#include "KeysView.h"

#include "modals/AddKey.h"
#include "modals/KeyDetail.h"
#include "modals/RegisterDevice.h"
#include "modules/hwi/HwiModule.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Section.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/feedback/Notice.h"
#include "catalog/feedback/Spinner.h"
#include "catalog/form/Field.h"
#include "catalog/inputs/ComboBox.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <QBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QVariant>
#include <algorithm>

#include <common.h>

namespace keys {

namespace {

const char *const K_FINGERPRINT = "keyFingerprint";
const char *const K_DEVICE_INSTANCE_ID = "deviceInstanceId";

auto stateBadge(plugin::SignerState state) -> catalog::Badge * {
    switch (state) {
    case plugin::SignerState::Ready:
        return catalog::pillDeviceReady();
    case plugin::SignerState::Locked:
        return catalog::pillDeviceLocked();
    default:
        return catalog::pillDeviceUnsupported();
    }
}

// The glyph says what a click does: a chevron opens, a plus registers. Null leaves
// the slot empty so the pills of both lists stay in one column.
auto glyphSlot(QWidget *glyph) -> QWidget * {
    auto *slot = new qontrol::Row;
    slot->setFixedWidth(metric::CHEVRON_SIZE);
    if (glyph != nullptr) {
        slot->push(glyph, 0, Qt::AlignCenter);
    }
    return slot;
}

auto glyphIcon(IconFn icon) -> QWidget * {
    auto *glyph = new QLabel;
    glyph->setPixmap(icon(IconColor::Muted).pixmap(metric::CHEVRON_SIZE, metric::CHEVRON_SIZE));
    return glyphSlot(glyph);
}

// The status pill, then the glyph, at the row end.
auto listEnd(catalog::Badge *pill, QWidget *glyph) -> QWidget * {
    return (new qontrol::Row)->spacing(resolve(Spacing::M))->pushSpacer()->push(pill)->push(glyph);
}

auto liveDevice(const QString &instance_id) -> std::optional<plugin::SignerEntry> {
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.instanceId == instance_id) {
            return entry;
        }
    }
    return std::nullopt;
}

auto routeSummary(const KeyInfo &info) -> QString {
    QStringList labels;
    for (const auto &route : info.routes) {
        labels.append(catalog::routeLabel(route));
    }
    if (labels.size() > 1) {
        labels[0] = TR("keys-route-primary-suffix").arg(labels[0]);
    }
    return labels.join(QStringLiteral(" · "));
}

} // namespace

KeysTab::KeysTab(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
}

void KeysTab::init() {
    m_network_selector = new catalog::ComboBox;
    for (auto network : {Network::Regtest, Network::Signet, Network::Testnet, Network::Bitcoin}) {
        m_network_selector->addItem(networkLabel(network), static_cast<int>(network));
    }
    m_network_selector->setWidth(Size::S);
    int index = m_network_selector->findData(static_cast<int>(KeysModule::network()));
    if (index >= 0) {
        m_network_selector->setCurrentIndex(index);
    }
    // The device service starts from its own config: align it with the network the
    // key records are on, so a device key is fetched for the network shown here.
    if (m_module->hwi() != nullptr) {
        m_module->hwi()->setNetwork(KeysModule::network());
    }

    m_error = new catalog::Notice(catalog::Notice::Tone::Error);
    auto *dismiss = catalog::btnDismissNotice();
    connect(dismiss, &QPushButton::clicked, this, &KeysTab::dismissError, qontrol::UNIQUE);
    m_error->setAction(dismiss);
    m_error->setVisible(false);

    m_device_column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    m_device_scroll = new catalog::ScrollArea;
    m_device_scroll->setFitContent(true);
    m_device_scroll->setWidget(m_device_column);
    // Outside the scroll area: the keys list below cannot squeeze it into a scroll.
    m_devices_empty = new catalog::EmptyState(icon::usb, TR("keys-devices-empty-title"));
    // Grows into the room the keys list leaves, up to twice its own height.
    m_devices_empty->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_devices = new catalog::Section(TR("keys-devices"));
    m_devices->header()->setFoldable(true);
    // Inset as the rows are by the scroll area's gutter: as wide as them.
    auto *devicesEmpty =
        (new qontrol::Row)
            ->margins(catalog::ScrollArea::insetH(), 0, catalog::ScrollArea::insetH(), 0)
            ->push(m_devices_empty);
    // Under each header, the design's gap before the list (its scroll area's padding).
    m_devices->setContent((new qontrol::Column)
                              ->margins(0, resolve(Spacing::XS), 0, 0)
                              ->push(m_device_scroll)
                              ->push(devicesEmpty));

    m_key_column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    m_key_scroll = new catalog::ScrollArea;
    m_key_scroll->setFitContent(true);
    m_key_scroll->setWidget(m_key_column);
    m_keys = new catalog::Section(TR("keys-keys"));
    m_keys->header()->setFoldable(true);
    m_keys->setContent(
        (new qontrol::Column)->margins(0, resolve(Spacing::XS), 0, 0)->push(m_key_scroll));
}

void KeysTab::doConnect() {
    connect(m_network_selector, &QComboBox::activated, this, &KeysTab::onNetworkChanged,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::keysChanged, this, &KeysTab::onKeysChanged, qontrol::UNIQUE);
    connect(m_module, &KeysModule::liveDevicesChanged, this, &KeysTab::onDevicesChanged,
            qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this, &KeysTab::onInstanceRemoved,
            qontrol::UNIQUE);
    connect(m_devices->header(), &catalog::SectionHeader::toggled, this, &KeysTab::onSectionToggled,
            qontrol::UNIQUE);
    connect(m_keys->header(), &catalog::SectionHeader::toggled, this, &KeysTab::onSectionToggled,
            qontrol::UNIQUE);
}

void KeysTab::view() {
    auto *title = (new qontrol::Row)
                      ->push(new catalog::Label(TR("keys-keys"), catalog::LabelRole::Title))
                      ->pushSpacer()
                      ->push(new catalog::Field(TR("keys-network"), m_network_selector));

    m_sections = (new qontrol::Column)->push(m_devices)->push(m_keys)->pushStretch(1);
    onSectionToggled();

    auto *column = (new qontrol::Column)
                       ->margins(resolve(Spacing::S))
                       ->spacing(resolve(Spacing::L))
                       ->push(title)
                       ->push(m_error)
                       ->push(m_sections, 1);
    column->setFixedWidth(metric::KEYS_TAB_WIDTH);

    auto *page = (new qontrol::Row)->pushSpacer()->push(column)->pushSpacer();
    setScreenContent(this, m_main_widget, page);
    rebuildDevices();
    rebuildKeys();
}

void KeysTab::showEvent(QShowEvent *event) {
    qontrol::Screen::showEvent(event);
    // Nothing polls the bus until someone is looking at it, or a key needs a device.
    m_module->setTabShown(true);
}

void KeysTab::hideEvent(QHideEvent *event) {
    qontrol::Screen::hideEvent(event);
    m_module->setTabShown(false);
}

void KeysTab::onNetworkChanged() {
    auto network = static_cast<Network>(m_network_selector->currentData().toInt());
    dismissError();
    // The hwi module owns the persistence, the keys module owns the records, and one
    // combobox drives both.
    m_module->setNetwork(network);
    if (m_module->hwi() != nullptr) {
        m_module->hwi()->setNetwork(network);
    }
    rebuildDevices();
    rebuildKeys();
}

void KeysTab::onDevicesChanged() {
    rebuildDevices();
    rebuildKeys();
}

void KeysTab::onKeysChanged() {
    rebuildDevices();
    rebuildKeys();
}

void KeysTab::onSectionToggled() {
    bool bothOpen = m_devices->header()->isOpen() && m_keys->header()->isOpen();
    m_sections->spacing(bothOpen ? resolve(Spacing::L) : resolve(Spacing::S));
}

void KeysTab::rebuildDevices() {
    m_device_column->clear();
    m_device_column->spacing(resolve(Spacing::S));

    int count = 0;
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer == nullptr) {
            continue;
        }
        ++count;
        auto *row = deviceRow(entry);
        m_device_column->push(row);
        // A widget added to a shown layout is only shown on the next event loop pass,
        // after the scroll area measured the column without it: show it now.
        row->show();
    }
    m_device_column->pushStretch(1);
    m_device_scroll->setVisible(count > 0);
    m_devices_empty->setVisible(count == 0);
    m_devices_empty->ensurePolished();
    int cardMinimum = m_devices_empty->minimumSizeHint().height();
    m_devices_empty->setMaximumHeight(2 * cardMinimum);
    // The section grows only as far as the card does.
    m_devices->setMaximumHeight(count == 0 ? m_devices->minimumSizeHint().height() + cardMinimum
                                           : QWIDGETSIZE_MAX);
    // Room left over goes to the empty card, never to a device list.
    auto *sections =
        m_sections != nullptr ? qobject_cast<QBoxLayout *>(m_sections->layout()) : nullptr;
    if (sections != nullptr) {
        // The card first, up to its cap; the trailing stretch takes the rest.
        sections->setStretchFactor(m_devices, count == 0 ? 1 : 0);
        sections->setStretch(sections->count() - 1, count == 0 ? 0 : 1);
    }
    m_devices->header()->setCount(count > 0 ? QString::number(count) : QString());
    m_device_scroll->updateGeometry();
}

auto KeysTab::deviceRow(const plugin::SignerEntry &entry) const -> catalog::SelectRow * {
    auto *signer = entry.signer;
    QString fingerprint = signer->fingerprint();
    QString name = deviceName(*signer);
    if (name.isEmpty()) {
        name = signer->id();
    }
    QString alias = keyAlias(fingerprint);
    bool known = !alias.isEmpty();
    bool ready = signer->state() == plugin::SignerState::Ready;
    bool registering = m_creations.contains(entry.instanceId);
    // Two devices of one seed would make the same key: one at a time per fingerprint.
    bool canRegister = ready && !fingerprint.isEmpty() && !known && !creating(fingerprint);

    QString sub;
    if (registering) {
        sub = TR("keys-device-registering");
    } else if (!ready) {
        sub = signer->stateDetail();
    } else if (known) {
        sub = TR("keys-device-registered-as").arg(alias);
    } else {
        sub = TR("keys-device-not-registered");
    }

    QWidget *glyph = nullptr;
    if (registering) {
        glyph = glyphSlot(new catalog::Spinner(metric::ROW_SPINNER_SIZE));
    } else if (canRegister) {
        glyph = glyphIcon(icon::plus);
    } else {
        glyph = glyphSlot(nullptr);
    }

    auto *row = new catalog::SelectRow;
    row->setIcon(icon::usb);
    row->setTitle(name);
    row->setMeta(fingerprint);
    row->setSub(sub);
    row->setStatus(listEnd(stateBadge(signer->state()), glyph));
    row->setStatusWidth(metric::LIST_STATUS_WIDTH);
    row->setChevron(false);
    row->setEnabled(!known);
    if (canRegister) {
        row->setProperty(K_DEVICE_INSTANCE_ID, entry.instanceId);
        connect(row, &catalog::SelectRow::clicked, this, &KeysTab::onDeviceClicked,
                qontrol::UNIQUE);
    } else {
        row->setClickable(false);
    }
    return row;
}

void KeysTab::rebuildKeys() {
    m_key_column->clear();
    m_key_column->spacing(resolve(Spacing::S));

    QList<KeyInfo> keyList = m_module->tabKeys();
    for (const auto &info : keyList) {
        auto *row = keyRow(info);
        m_key_column->push(row);
        row->show();
    }
    if (keyList.isEmpty()) {
        auto *addKey = catalog::btnAddAKey();
        connect(addKey, &QPushButton::clicked, this, &KeysTab::openAddKey, qontrol::UNIQUE);
        auto *empty = new catalog::EmptyState(
            icon::key, TR("keys-keys-empty-title").arg(networkLabel(KeysModule::network())));
        empty->setAction(addKey);
        m_key_column->push(empty);
        empty->show();
        m_keys->header()->setAction(nullptr);
    } else {
        auto *addKey = catalog::btnAddAKey();
        connect(addKey, &QPushButton::clicked, this, &KeysTab::openAddKey, qontrol::UNIQUE);
        // Inset as the rows are by the scroll area's gutter, so its right edge meets theirs.
        m_keys->header()->setAction(
            (new qontrol::Row)->margins(0, 0, catalog::ScrollArea::insetH(), 0)->push(addKey));
    }
    m_key_column->pushStretch(1);
    m_keys->header()->setCount(keyList.isEmpty() ? QString() : QString::number(keyList.size()));
    m_key_scroll->updateGeometry();
}

auto KeysTab::keyRow(const KeyInfo &info) const -> catalog::SelectRow * {
    QString fingerprint = KeysModule::toQt(info.fingerprint);
    auto *row = new catalog::SelectRow;
    row->setIcon(info.routes.empty() ? icon::key : catalog::routeIcon(info.routes[0]));
    row->setTitle(KeysModule::toQt(info.alias));
    row->setMeta(fingerprint);
    row->setSub(routeSummary(info));
    row->setStatus(listEnd(keyStatusPill(info), glyphIcon(icon::chevronRight)));
    row->setStatusWidth(metric::LIST_STATUS_WIDTH);
    row->setChevron(false);
    row->setProperty(K_FINGERPRINT, fingerprint);
    connect(row, &catalog::SelectRow::clicked, this, &KeysTab::onKeyClicked, qontrol::UNIQUE);
    return row;
}

auto KeysTab::keyAlias(const QString &fingerprint) const -> QString {
    if (fingerprint.isEmpty()) {
        return QString();
    }
    for (const auto &info : m_module->tabKeys()) {
        if (QString::compare(KeysModule::toQt(info.fingerprint), fingerprint,
                             Qt::CaseInsensitive) == 0) {
            return KeysModule::toQt(info.alias);
        }
    }
    return QString();
}

void KeysTab::onDeviceClicked() {
    auto *row = sender();
    if (row == nullptr) {
        return;
    }
    // Re-query rather than capturing the signer pointer: a device can vanish between the
    // rebuild and the click.
    auto entry = liveDevice(row->property(K_DEVICE_INSTANCE_ID).toString());
    if (!entry.has_value()) {
        return;
    }
    QStringList aliases;
    for (const auto &info : m_module->tabKeys()) {
        aliases.append(KeysModule::toQt(info.alias));
    }
    if (creating(entry->signer->fingerprint())) {
        return;
    }
    m_register_instance = entry->instanceId;
    auto *modal =
        new modal::RegisterDevice(deviceName(*entry->signer), entry->signer->fingerprint(),
                                  networkLabel(KeysModule::network()), aliases);
    connect(modal, &modal::RegisterDevice::registerConfirmed, this, &KeysTab::onRegisterConfirmed,
            qontrol::UNIQUE);
    AppController::execModal(modal);
}

void KeysTab::onRegisterConfirmed(const QString &alias) {
    dismissError();
    auto entry = liveDevice(m_register_instance);
    if (!entry.has_value()) {
        showRegisterError(QString(), TR("keys-register-disconnected"));
        return;
    }
    if (m_creations.contains(entry->instanceId) || creating(entry->signer->fingerprint())) {
        return;
    }
    KeyCreation creation{
        .name = deviceName(*entry->signer),
        .alias = alias,
        .paths = KeysModule::deviceXpubPaths(KeysModule::deviceCapabilities(*entry)),
        .xpubs = {},
        .signer = entry->signer,
        .req = 0,
    };
    connect(entry->signer, &ISigner::xpub, this, &KeysTab::onRegisterXpub, qontrol::UNIQUE);
    connect(entry->signer, &ISigner::error, this, &KeysTab::onRegisterError, qontrol::UNIQUE);
    auto &stored = m_creations.insert(entry->instanceId, creation).value();
    requestRegisterXpub(stored);
    rebuildDevices();
}

void KeysTab::requestRegisterXpub(KeyCreation &creation) {
    creation.req = creation.signer->getXpub(creation.paths.at(creation.xpubs.size()));
}

auto KeysTab::creationFor(const QObject *signer, ReqId req_id) const -> QString {
    for (auto it = m_creations.cbegin(); it != m_creations.cend(); ++it) {
        if (it->req != 0 && it->req == req_id && it->signer.data() == signer) {
            return it.key();
        }
    }
    return {};
}

auto KeysTab::creating(const QString &fingerprint) const -> bool {
    return std::ranges::any_of(m_creations, [&fingerprint](const KeyCreation &creation) {
        return !creation.signer.isNull() &&
               creation.signer->fingerprint().compare(fingerprint, Qt::CaseInsensitive) == 0;
    });
}

void KeysTab::onRegisterXpub(ReqId req_id, const QString &xpub) {
    auto instance = creationFor(sender(), req_id);
    if (instance.isEmpty()) {
        return;
    }
    auto &creation = m_creations[instance];
    creation.xpubs.append({.path = creation.paths.at(creation.xpubs.size()), .xpub = xpub});
    if (creation.xpubs.size() < creation.paths.size()) {
        requestRegisterXpub(creation);
        return;
    }
    auto done = m_creations.value(instance);
    finishRegistering(instance);
    auto entry = liveDevice(instance);
    if (!entry.has_value()) {
        showRegisterError(done.name, TR("keys-register-disconnected"));
        return;
    }
    auto result = m_module->addDeviceKey(entry.value(), done.alias, done.xpubs);
    if (!result.is_ok) {
        showRegisterError(done.name, KeysModule::toQt(result.error));
    }
}

void KeysTab::onRegisterError(std::optional<ReqId> req_id, const QString &message) {
    if (!req_id.has_value()) {
        return;
    }
    auto instance = creationFor(sender(), req_id.value());
    if (instance.isEmpty()) {
        return;
    }
    auto name = m_creations.value(instance).name;
    finishRegistering(instance);
    showRegisterError(name, message);
}

void KeysTab::onInstanceRemoved(const QString &id) {
    if (!m_creations.contains(id)) {
        return;
    }
    auto name = m_creations.value(id).name;
    finishRegistering(id);
    showRegisterError(name, TR("keys-register-disconnected"));
}

void KeysTab::finishRegistering(const QString &instance_id) {
    auto creation = m_creations.take(instance_id);
    // One creation per device: its signer serves no other.
    if (!creation.signer.isNull()) {
        disconnect(creation.signer, nullptr, this, nullptr);
    }
    rebuildDevices();
}

void KeysTab::showRegisterError(const QString &device, const QString &message) {
    m_error->setTitle(TR("keys-register-failed-title").arg(device));
    m_error->setText(message);
    m_error->setVisible(true);
}

void KeysTab::dismissError() {
    m_error->setVisible(false);
}

void KeysTab::onKeyClicked() {
    auto *row = sender();
    if (row == nullptr) {
        return;
    }
    QString fingerprint = row->property(K_FINGERPRINT).toString();
    if (fingerprint.isEmpty() || m_module->rust() == nullptr) {
        return;
    }
    AppController::execModal(new modal::KeyDetail(m_module, fingerprint));
}

void KeysTab::openAddKey() {
    AppController::execModal(new modal::AddKey(m_module));
}

} // namespace keys
