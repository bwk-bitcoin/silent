#include "KeyDetail.h"

#include "FetchXpub.h"
#include "RevealMnemonic.h"
#include "Unseal.h"
#include "keydetail/KeyRecord.h"
#include "keydetail/RemoveFlow.h"

#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Section.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/display/ShortValue.h"
#include "catalog/feedback/Notice.h"
#include "catalog/feedback/Spinner.h"
#include "catalog/form/Field.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QDebug>
#include <QHash>
#include <QLabel>

#include <algorithm>
#include <utility>

#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

const char *const K_ROUTE_INDEX = "routeIndex";
const char *const K_DESCRIPTOR = "descriptor";
const char *const K_XPUB_PATH = "xpubPath";

// The route kinds a key can be reached over, in the order the picker offers them.
const QList<KeyRouteKind> ROUTE_KINDS = {KeyRouteKind::Hot, KeyRouteKind::Usb, KeyRouteKind::Qr,
                                         KeyRouteKind::SdCard};

auto emptyLine(const QString &text) -> Label * {
    auto *label = new Label(text, LabelRole::Body);
    label->setTone(LabelTone::Muted);
    label->setWordWrap(true);
    return label;
}

// The account column leads the row, in demibold as the design sets it.
auto accountName(const QString &name) -> Label * {
    auto *label = new Label(name, LabelRole::Body);
    label->setWeight(font::DEMIBOLD);
    return label;
}

auto routeIconLabel(KeyRouteKind kind) -> QWidget * {
    auto *icon = new QLabel;
    icon->setPixmap(catalog::routeIcon(kind)(IconColor::Accent)
                        .pixmap(metric::ROUTE_ICON_SIZE, metric::ROUTE_ICON_SIZE));
    return icon;
}

auto denseSection(const QString &label, bool open) -> catalog::Section * {
    auto *section = new catalog::Section(label);
    section->header()->setFoldable(true);
    section->header()->setDense(true);
    section->header()->setOpen(open);
    return section;
}

auto countText(int count) -> QString {
    return count > 0 ? QString::number(count) : QString();
}

// A hot key or an SD card export holds no wallet policy: only a device reached over
// USB or QR registers one.
auto registrationApplies(const KeyInfo &info) -> bool {
    return std::ranges::any_of(info.routes, [](KeyRouteKind route) {
        return route == KeyRouteKind::Usb || route == KeyRouteKind::Qr;
    });
}

} // namespace

KeyDetail::KeyDetail(KeysModule *module, const QString &fingerprint)
    : m_module(module),
      m_fingerprint(fingerprint) {
    init();
    doConnect();
    view();
    onKeyChanged();
    showDetail();
}

void KeyDetail::done(int result) {
    closeFlow();
    catalog::Modal::done(result);
}

void KeyDetail::init() {
    auto current = keyInfo(m_module, m_fingerprint);
    m_alias = current.has_value() ? KeysModule::toQt(current->alias) : QString();
    initDetail();
}

void KeyDetail::doConnect() {
    connect(m_module, &KeysModule::keysChanged, this, &KeyDetail::onKeyChanged, qontrol::UNIQUE);
    connect(m_module, &KeysModule::liveDevicesChanged, this, &KeyDetail::onKeyChanged,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::mnemonicRevealed, this, &KeyDetail::onMnemonicRevealed,
            qontrol::UNIQUE);
    connectDetail();
}

void KeyDetail::view() {
    m_detail = viewDetail();
}

// ===== Detail view =====

void KeyDetail::initDetail() {
    m_alias_input = new catalog::Input(m_alias);
    m_save_alias = catalog::btnSaveAlias();
    m_save_alias->setVisible(false);
    m_alias_note = new catalog::FieldNote;
    m_alias_note->setVisible(false);
    m_route_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_route_error->setVisible(false);

    m_devices_table = new catalog::Table;
    m_devices_table->addColumn({.title = TR("keys-device-column"), .grow = true});
    m_devices_table->addColumn(
        {.title = TR("keys-module-column"), .width = metric::ACCOUNT_MODULE_COLUMN});
    m_devices_empty = emptyLine(TR("keys-devices-seen-empty"));

    m_routes_table = new catalog::Table;
    m_routes_table->setHeaderVisible(false);
    m_routes_table->addColumn({.width = metric::ROUTE_ICON_COLUMN});
    m_routes_table->addColumn({.grow = true});
    m_routes_table->addColumn({.width = metric::ROUTE_ACTIONS_COLUMN, .alignment = Qt::AlignRight});

    m_xpubs_table = new catalog::Table;
    m_xpubs_table->setHeaderVisible(false);
    m_xpubs_table->addColumn({.width = metric::XPUB_PATH_COLUMN});
    m_xpubs_table->addColumn({.grow = true});
    m_xpubs_table->addColumn({.width = metric::XPUB_PILL_COLUMN, .alignment = Qt::AlignRight});
    m_xpubs_table->addColumn({.width = metric::XPUB_REMOVE_COLUMN, .alignment = Qt::AlignRight});

    m_accounts_table = new catalog::Table;
    m_accounts_table->setHeaderVisible(false);
    m_accounts_table->addColumn({.grow = true});
    m_accounts_table->addColumn({.width = metric::ACCOUNT_MODULE_COLUMN});
    m_accounts_table->addColumn(
        {.width = metric::ACCOUNT_PILL_COLUMN, .alignment = Qt::AlignRight});
    m_accounts_empty = emptyLine(TR("keys-accounts-empty"));
    m_register_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_register_error->setVisible(false);

    m_reveal = catalog::btnRevealMnemonic();
    m_remove = catalog::btnRemoveKey();
    m_detail_actions = new catalog::ModalActions;
    m_detail_actions->addAction(m_reveal);
    m_detail_actions->addAction(m_remove);
}

void KeyDetail::connectDetail() {
    connect(m_alias_input, &QLineEdit::textEdited, this, &KeyDetail::onAliasEdited,
            qontrol::UNIQUE);
    connect(m_alias_input, &QLineEdit::returnPressed, this, &KeyDetail::onSaveAliasClicked,
            qontrol::UNIQUE);
    connect(m_save_alias, &QPushButton::clicked, this, &KeyDetail::onSaveAliasClicked,
            qontrol::UNIQUE);
    for (auto *table : {m_devices_table, m_routes_table, m_xpubs_table, m_accounts_table}) {
        connect(table, &catalog::Table::rebuilt, this, &KeyDetail::adjustSize, qontrol::UNIQUE);
    }
    connect(m_reveal, &QPushButton::clicked, this, &KeyDetail::onRevealClicked, qontrol::UNIQUE);
    connect(m_remove, &QPushButton::clicked, this, &KeyDetail::onRemoveClicked, qontrol::UNIQUE);
}

auto KeyDetail::viewDetail() -> catalog::ScrollArea * {
    auto *aliasRow = (new qontrol::Row)
                         ->spacing(resolve(Spacing::XS))
                         ->push(m_alias_input, 1)
                         ->push(m_save_alias);
    auto *alias = (new qontrol::Column)
                      ->spacing(resolve(Spacing::S))
                      ->push(new catalog::Field(TR("keys-alias"), aliasRow))
                      ->push(m_alias_note);

    m_devices_section = denseSection(TR("keys-devices-seen"), false);
    m_devices_section->setContent(
        (new qontrol::Column)->push(m_devices_table)->push(m_devices_empty));
    m_routes_section = denseSection(TR("keys-routes"), true);
    m_routes_section->setContent(m_routes_table);
    m_xpubs_section = denseSection(TR("keys-cached-xpubs"), true);
    m_xpubs_section->setContent(m_xpubs_table);
    m_accounts_section = denseSection(TR("keys-accounts"), true);
    m_accounts_section->setContent((new qontrol::Column)
                                       ->spacing(resolve(Spacing::S))
                                       ->push(m_accounts_table)
                                       ->push(m_accounts_empty)
                                       ->push(m_register_error));
    for (auto *section :
         {m_devices_section, m_routes_section, m_xpubs_section, m_accounts_section}) {
        connect(section->header(), &catalog::SectionHeader::toggled, this, &KeyDetail::adjustSize,
                qontrol::UNIQUE);
    }

    auto *column = (new qontrol::Column)
                       ->spacing(resolve(Spacing::XS))
                       ->push(alias)
                       ->push(m_route_error)
                       ->push(m_devices_section)
                       ->push(m_routes_section)
                       ->push(m_xpubs_section)
                       ->push(m_accounts_section);
    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    scroll->setWidget(column);
    return scroll;
}

void KeyDetail::showDetail() {
    showHeader(m_alias);
    setBody(m_detail);
    setFooter(m_detail_actions);
    setWidth(ModalWidth::L);
}

void KeyDetail::refreshDevices() {
    auto devices = m_module->rust()->key_devices(rust::String(m_fingerprint.toStdString()));
    m_devices_table->clearRows();
    int index = 0;
    for (const auto &device : devices) {
        m_devices_table->addRow(
            QString::number(index++),
            {deviceName(KeysModule::toQt(device.kind), KeysModule::toQt(device.model)),
             KeysModule::toQt(device.module)});
    }
    m_devices_table->setVisible(!devices.empty());
    m_devices_empty->setVisible(devices.empty());
    m_devices_section->header()->setCount(countText(static_cast<int>(devices.size())));
}

void KeyDetail::refreshRoutes(const KeyInfo &info) {
    QList<KeyRouteKind> routes;
    for (const auto &route : info.routes) {
        routes.append(route);
    }
    m_routes_table->clearRows();
    m_route_picker = nullptr;
    for (int index = 0; index < routes.size(); ++index) {
        KeyRouteKind kind = routes.at(index);
        // The hot route stays first: the key record refuses any other order.
        bool hotAbove = index > 0 && routes.at(index - 1) == KeyRouteKind::Hot;
        auto *up = catalog::btnMoveUp();
        up->setProperty(K_ROUTE_INDEX, index);
        up->setEnabled(index > 0 && !hotAbove);
        connect(up, &QPushButton::clicked, this, &KeyDetail::onMoveUpClicked, qontrol::UNIQUE);
        auto *down = catalog::btnMoveDown();
        down->setProperty(K_ROUTE_INDEX, index);
        down->setEnabled(index < routes.size() - 1 && kind != KeyRouteKind::Hot);
        connect(down, &QPushButton::clicked, this, &KeyDetail::onMoveDownClicked, qontrol::UNIQUE);
        auto *remove = catalog::btnRemoveRoute();
        remove->setProperty(K_ROUTE_INDEX, index);
        bool last = routes.size() == 1;
        remove->setEnabled(!last);
        remove->setToolTip(last ? TR("keys-route-last") : TR("keys-remove-route"));
        connect(remove, &QPushButton::clicked, this, &KeyDetail::onRemoveRouteClicked,
                qontrol::UNIQUE);
        auto *actions =
            (new qontrol::Row)->spacing(resolve(Spacing::XXS))->push(up)->push(down)->push(remove);
        m_routes_table->addWidgetRow(
            QString::number(index),
            {routeIconLabel(kind), new Label(catalog::routeLabel(kind), LabelRole::Body), actions});
    }

    QList<KeyRouteKind> addable;
    for (auto kind : ROUTE_KINDS) {
        if (!routes.contains(kind) && (kind != KeyRouteKind::Hot || info.has_hot_secret)) {
            addable.append(kind);
        }
    }
    m_routes_section->header()->setCount(countText(static_cast<int>(routes.size())));
    if (addable.isEmpty()) {
        m_adding_route = false;
        return;
    }
    // The last row adds a route: + opens a kind picker in the row and becomes a check
    // to confirm, with a cross beside it to cancel.
    auto *actions = (new qontrol::Row)->spacing(resolve(Spacing::XXS));
    if (m_adding_route) {
        m_route_picker = new catalog::ComboBox;
        for (auto kind : addable) {
            m_route_picker->addItem(catalog::routeLabel(kind), static_cast<int>(kind));
        }
        m_route_picker->setWidth(Size::S);
        auto *cancel = catalog::btnCancelAddRoute();
        connect(cancel, &QPushButton::clicked, this, &KeyDetail::onCancelRouteClicked,
                qontrol::UNIQUE);
        auto *confirm = catalog::btnConfirmAddRoute();
        connect(confirm, &QPushButton::clicked, this, &KeyDetail::onConfirmRouteClicked,
                qontrol::UNIQUE);
        actions->push(cancel)->push(confirm);
    } else {
        auto *add = catalog::btnAddRouteIcon();
        connect(add, &QPushButton::clicked, this, &KeyDetail::onAddRouteClicked, qontrol::UNIQUE);
        actions->push(add);
    }
    m_routes_table->addWidgetRow(QStringLiteral("add"), {nullptr, m_route_picker, actions},
                                 !m_adding_route);
}

void KeyDetail::moveRoute(int from_index, int to_index) {
    auto current = keyInfo(m_module, m_fingerprint);
    if (!current.has_value()) {
        return;
    }
    rust::Vec<KeyRouteKind> order = current->routes;
    std::swap(order[static_cast<std::size_t>(from_index)],
              order[static_cast<std::size_t>(to_index)]);
    showRouteResult(m_module->setRouteOrder(m_fingerprint, std::move(order)));
}

void KeyDetail::showRouteResult(const KeyOpResult &result) {
    m_route_error->setVisible(!result.is_ok);
    if (!result.is_ok) {
        m_route_error->setText(TR("keys-route-failed").arg(KeysModule::toQt(result.error)));
        // A failed change leaves the record as it was: show it as it is.
        onKeyChanged();
    }
}

void KeyDetail::refreshXpubs() {
    auto fingerprint = rust::String(m_fingerprint.toStdString());
    auto xpubs = m_module->rust()->key_xpubs(fingerprint);
    auto descriptors = m_module->rust()->key_descriptors(fingerprint);
    m_xpubs_table->clearRows();
    for (const auto &entry : xpubs) {
        QString xpub = KeysModule::toQt(entry.xpub);
        // An xpub is used once a descriptor of this key carries it.
        bool used = false;
        for (const auto &descriptor : descriptors) {
            if (KeysModule::toQt(descriptor.descriptor).contains(xpub)) {
                used = true;
                break;
            }
        }
        auto *path = new Label(xpubPath(KeysModule::toQt(entry.path)), LabelRole::Mono);
        path->setTone(LabelTone::Secondary);
        // An unused xpub can leave the cache; a used one is held by its account.
        auto *remove = catalog::btnRemoveXpub();
        remove->setProperty(K_XPUB_PATH, KeysModule::toQt(entry.path));
        remove->setEnabled(!used);
        remove->setToolTip(used ? TR("keys-xpub-used") : TR("keys-remove-xpub"));
        connect(remove, &QPushButton::clicked, this, &KeyDetail::onRemoveXpubClicked,
                qontrol::UNIQUE);
        m_xpubs_table->addWidgetRow(
            xpub, {path, new catalog::ShortValue(xpub),
                   used ? catalog::pillXpubUsed() : catalog::pillXpubUnused(), remove});
    }

    auto device = readyDevice(m_fingerprint);
    // A hot key derives its xpubs itself, with its password: no device needed.
    auto info = keyInfo(m_module, m_fingerprint);
    bool hot = info.has_value() && info->has_hot_secret;
    bool fetchable = device.has_value() || hot;
    Label *empty = nullptr;
    if (xpubs.empty()) {
        empty = emptyLine(fetchable ? TR("keys-xpubs-empty") : TR("keys-xpubs-empty-offline"));
    }
    auto *fetch = catalog::btnFetchXpub();
    fetch->setEnabled(fetchable);
    if (device.has_value()) {
        fetch->setToolTip(TR("keys-fetch-xpub-from").arg(deviceName(*device->signer)));
    } else if (hot) {
        fetch->setToolTip(TR("keys-fetch-xpub-hot"));
    } else {
        fetch->setToolTip(TR("keys-fetch-xpub-connect"));
    }
    connect(fetch, &QPushButton::clicked, this, &KeyDetail::onFetchXpubClicked, qontrol::UNIQUE);
    m_xpubs_table->addWidgetRow(QStringLiteral("add"), {nullptr, empty, fetch, nullptr}, true);
    m_xpubs_section->header()->setCount(countText(static_cast<int>(xpubs.size())));
}

void KeyDetail::refreshAccounts(const KeyInfo &info) {
    QList<KeyAccount> list = keyAccounts(m_module, m_fingerprint);
    bool applies = registrationApplies(info);
    syncRegistrations(list, applies);
    m_accounts_table->clearRows();
    for (const auto &account : list) {
        auto *module = new Label(TR("keys-accounts-module-sp"), LabelRole::Body);
        module->setTone(LabelTone::Muted);
        m_accounts_table->addWidgetRow(account.descriptor, {accountName(account.name), module,
                                                            registrationCell(account, applies)});
    }
    m_accounts_table->setVisible(!list.isEmpty());
    m_accounts_empty->setVisible(list.isEmpty());
    m_accounts_section->header()->setCount(countText(static_cast<int>(list.size())));
}

auto KeyDetail::registrationCell(const KeyAccount &account, bool applies) -> QWidget * {
    if (!applies) {
        auto *label = new Label(TR("keys-account-registration-not-needed"), LabelRole::Body);
        label->setTone(LabelTone::Muted);
        return label;
    }
    switch (m_registrations.value(account.descriptor, Registration::Unknown)) {
    case Registration::Registered:
        return catalog::pillAccountRegistered();
    case Registration::Checking:
    case Registration::Registering:
        return new catalog::Spinner(metric::ROW_SPINNER_SIZE);
    case Registration::Unknown:
    case Registration::NotRegistered:
        break;
    }
    auto *registerAccount = catalog::btnRegisterAccount();
    registerAccount->setProperty(K_DESCRIPTOR, account.descriptor);
    registerAccount->setEnabled(!m_register_signer.isNull());
    registerAccount->setToolTip(
        m_register_signer.isNull()
            ? TR("keys-register-account-connect")
            : TR("keys-register-account-on").arg(deviceName(*m_register_signer)));
    connect(registerAccount, &QPushButton::clicked, this, &KeyDetail::onRegisterClicked,
            qontrol::UNIQUE);
    return registerAccount;
}

void KeyDetail::syncRegistrations(const QList<KeyAccount> &accounts, bool applies) {
    auto device = applies ? readyDevice(m_fingerprint) : std::nullopt;
    QString instance = device.has_value() ? device->instanceId : QString();
    if (instance != m_register_instance) {
        stopRegistrations();
        m_register_instance = instance;
        if (device.has_value()) {
            m_register_signer = device->signer;
            connect(device->signer, &ISigner::descriptorIsRegistered, this,
                    &KeyDetail::onDescriptorIsRegistered, qontrol::UNIQUE);
            connect(device->signer, &ISigner::descriptorRegistered, this,
                    &KeyDetail::onDescriptorRegistered, qontrol::UNIQUE);
            connect(device->signer, &ISigner::error, this, &KeyDetail::onRegistrationError,
                    qontrol::UNIQUE);
        }
    }
    if (m_register_signer.isNull()) {
        return;
    }
    for (const auto &account : accounts) {
        if (!m_registrations.contains(account.descriptor)) {
            m_registrations.insert(account.descriptor, Registration::Checking);
            m_register_queue.append(account.descriptor);
        }
    }
    sendNextRegistration();
}

void KeyDetail::sendNextRegistration() {
    if (m_register_req != 0 || m_register_queue.isEmpty() || m_register_signer.isNull()) {
        return;
    }
    m_register_descriptor = m_register_queue.takeFirst();
    m_register_req = m_registrations.value(m_register_descriptor) == Registration::Registering
                         ? m_register_signer->registerDescriptor(m_register_descriptor)
                         : m_register_signer->isDescriptorRegistered(m_register_descriptor);
}

void KeyDetail::finishRegistrationRequest(Registration state) {
    m_registrations.insert(m_register_descriptor, state);
    m_register_descriptor.clear();
    m_register_req = 0;
    // Rebuilds the rows, which sends the next queued request.
    onKeyChanged();
}

void KeyDetail::stopRegistrations() {
    if (!m_register_signer.isNull()) {
        disconnect(m_register_signer, &ISigner::descriptorIsRegistered, this,
                   &KeyDetail::onDescriptorIsRegistered);
        disconnect(m_register_signer, &ISigner::descriptorRegistered, this,
                   &KeyDetail::onDescriptorRegistered);
        disconnect(m_register_signer, &ISigner::error, this, &KeyDetail::onRegistrationError);
    }
    m_register_signer = nullptr;
    m_register_instance.clear();
    m_registrations.clear();
    m_register_queue.clear();
    m_register_descriptor.clear();
    m_register_req = 0;
    m_register_error->setVisible(false);
}

void KeyDetail::showRegistrationError(const QString &message) {
    m_register_error->setText(message);
    m_register_error->setVisible(true);
}

// ===== Shared =====

void KeyDetail::showHeader(const QString &title) {
    auto current = keyInfo(m_module, m_fingerprint);
    if (!current.has_value()) {
        return;
    }
    setWindowTitle(title);
    auto *old = m_header;
    m_header = keyHeader(title, m_fingerprint, current.value());
    setHeader(m_header);
    delete old;
}

void KeyDetail::openFlow(KeyDetailFlow *flow) {
    m_flow = flow;
    connect(m_flow, &KeyDetailFlow::viewChanged, this, &KeyDetail::onFlowViewChanged,
            qontrol::UNIQUE);
    connect(m_flow, &KeyDetailFlow::back, this, &KeyDetail::onFlowBack, qontrol::UNIQUE);
    connect(m_flow, &KeyDetailFlow::removed, this, &KeyDetail::accept, qontrol::UNIQUE);
    setPage(m_flow);
    m_flow->start();
}

void KeyDetail::closeFlow() {
    if (m_flow == nullptr) {
        return;
    }
    // Deleted later: the flow may still be in the slot that emitted.
    m_flow->deleteLater();
    m_flow = nullptr;
}

// ===== Slots =====

void KeyDetail::onKeyChanged() {
    auto current = keyInfo(m_module, m_fingerprint);
    if (!current.has_value()) {
        // Removed: the window is closing.
        return;
    }
    m_alias = KeysModule::toQt(current->alias);
    // The detail's title is the alias, which may have just changed. A flow keeps its own.
    if (m_flow == nullptr) {
        showHeader(m_alias);
    }
    onAliasEdited();
    refreshDevices();
    refreshRoutes(current.value());
    refreshXpubs();
    refreshAccounts(current.value());
    m_reveal->setVisible(current->has_hot_secret);
}

void KeyDetail::onAliasEdited() {
    bool changed = m_alias_input->text().trimmed() != m_alias;
    m_save_alias->setVisible(changed);
    if (changed && m_alias_note->isVisible()) {
        m_alias_note->setVisible(false);
        adjustSize();
    }
}

void KeyDetail::onSaveAliasClicked() {
    QString alias = m_alias_input->text().trimmed();
    if (alias == m_alias) {
        return;
    }
    QString error;
    if (alias.isEmpty()) {
        error = TR("keys-alias-empty");
    }
    for (const auto &other : m_module->keys()) {
        if (error.isEmpty() && KeysModule::toQt(other.fingerprint) != m_fingerprint &&
            KeysModule::toQt(other.alias) == alias) {
            error = TR("keys-alias-taken").arg(alias);
        }
    }
    if (error.isEmpty()) {
        auto result = m_module->setAlias(m_fingerprint, alias);
        if (!result.is_ok) {
            error = TR("keys-alias-failed").arg(KeysModule::toQt(result.error));
        }
    }
    // A saved alias needs no message: the save button going away says it.
    m_alias_note->setTone(catalog::FieldNote::Tone::Error);
    m_alias_note->setText(error);
    m_alias_note->setVisible(!error.isEmpty());
    adjustSize();
}

void KeyDetail::onMoveUpClicked() {
    int index = sender()->property(K_ROUTE_INDEX).toInt();
    moveRoute(index, index - 1);
}

void KeyDetail::onMoveDownClicked() {
    int index = sender()->property(K_ROUTE_INDEX).toInt();
    moveRoute(index, index + 1);
}

void KeyDetail::onRemoveRouteClicked() {
    auto current = keyInfo(m_module, m_fingerprint);
    if (!current.has_value()) {
        return;
    }
    auto index = static_cast<std::size_t>(sender()->property(K_ROUTE_INDEX).toInt());
    showRouteResult(m_module->removeRoute(m_fingerprint, current->routes[index]));
}

void KeyDetail::onRemoveXpubClicked() {
    QString path = sender()->property(K_XPUB_PATH).toString();
    auto result = m_module->removeXpub(m_fingerprint, path);
    if (!result.is_ok) {
        qWarning() << "KeyDetail: removing xpub" << path
                   << "failed:" << KeysModule::toQt(result.error);
    }
}

void KeyDetail::onAddRouteClicked() {
    m_adding_route = true;
    onKeyChanged();
}

void KeyDetail::onConfirmRouteClicked() {
    if (m_route_picker == nullptr) {
        return;
    }
    auto kind = static_cast<KeyRouteKind>(m_route_picker->currentData().toInt());
    m_adding_route = false;
    auto result = m_module->addRoute(m_fingerprint, kind);
    showRouteResult(result);
}

void KeyDetail::onCancelRouteClicked() {
    m_adding_route = false;
    onKeyChanged();
}

void KeyDetail::onFetchXpubClicked() {
    // Over this modal, which shows the new xpub once the key records it.
    AppController::execModal(new FetchXpub(m_module, m_fingerprint));
}

void KeyDetail::onRegisterClicked() {
    QString descriptor = sender()->property(K_DESCRIPTOR).toString();
    m_registrations.insert(descriptor, Registration::Registering);
    m_register_queue.append(descriptor);
    m_register_error->setVisible(false);
    onKeyChanged();
}

void KeyDetail::onDescriptorIsRegistered(ReqId req_id, bool registered) {
    if (m_register_req == 0 || req_id != m_register_req) {
        return;
    }
    finishRegistrationRequest(registered ? Registration::Registered : Registration::NotRegistered);
}

void KeyDetail::onDescriptorRegistered(ReqId req_id, bool registered) {
    if (m_register_req == 0 || req_id != m_register_req) {
        return;
    }
    if (!registered) {
        showRegistrationError(TR("keys-register-account-refused"));
    }
    finishRegistrationRequest(registered ? Registration::Registered : Registration::NotRegistered);
}

void KeyDetail::onRegistrationError(std::optional<ReqId> req_id, const QString &message) {
    if (m_register_req == 0 || !req_id.has_value() || req_id.value() != m_register_req) {
        return;
    }
    bool registering = m_registrations.value(m_register_descriptor) == Registration::Registering;
    showRegistrationError(registering ? TR("keys-register-account-failed").arg(message)
                                      : TR("keys-check-account-failed").arg(message));
    finishRegistrationRequest(registering ? Registration::NotRegistered : Registration::Unknown);
}

// The password over the detail, then the words in a modal of their own.
void KeyDetail::onRevealClicked() {
    auto *unseal = new Unseal(TR("keys-reveal-title"), QString(), catalog::btnShowMnemonic());
    unseal->setBusy(TR("keys-reveal-busy"), QString());
    connect(unseal, &Unseal::passwordEntered, this, &KeyDetail::onRevealPassword, qontrol::UNIQUE);
    m_unseal = unseal;
    bool opened = unseal->exec() == QDialog::Accepted;
    delete unseal;
    m_reveal_req = 0;
    QStringList words = m_revealed;
    m_revealed.clear();
    if (opened) {
        AppController::execModal(new RevealMnemonic(words));
    }
}

void KeyDetail::onRevealPassword(const QString &password) {
    m_reveal_req = m_module->nextReqId();
    m_module->revealMnemonic(m_reveal_req, m_fingerprint, password);
}

void KeyDetail::onMnemonicRevealed(ReqId req_id, const MnemonicResult &result) {
    if (m_reveal_req == 0 || req_id != m_reveal_req || m_unseal.isNull()) {
        return;
    }
    m_reveal_req = 0;
    if (!result.is_ok) {
        m_unseal->retry(result.wrong_password
                            ? TR("flow-password-wrong")
                            : TR("keys-reveal-failed").arg(KeysModule::toQt(result.error)));
        return;
    }
    // A QString cannot be zeroized: Rust owns the real wiping of the phrase.
    QString mnemonic = KeysModule::toQt(result.mnemonic);
    m_revealed = mnemonic.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    mnemonic.fill(QChar());
    m_unseal->finish(true);
}

void KeyDetail::onRemoveClicked() {
    openFlow(new RemoveFlow(m_module, m_fingerprint));
}

void KeyDetail::onFlowViewChanged(const QString &title, ModalWidth width) {
    setWindowTitle(title);
    setWidth(width);
}

void KeyDetail::onFlowBack() {
    showDetail();
    closeFlow();
}

// ===== KeyDetailFlow =====

KeyDetailFlow::KeyDetailFlow(KeysModule *module, QString fingerprint)
    : m_module(module),
      m_fingerprint(std::move(fingerprint)) {
    m_slots = new catalog::ViewSlots;
    (new qontrol::Column)->push(m_slots)->into(this);
    connect(m_module, &KeysModule::keysChanged, this, &KeyDetailFlow::onKeyChanged,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::liveDevicesChanged, this, &KeyDetailFlow::onKeyChanged,
            qontrol::UNIQUE);
}

auto KeyDetailFlow::viewSlots() const -> catalog::ViewSlots * {
    return m_slots;
}

auto KeyDetailFlow::module() const -> KeysModule * {
    return m_module;
}

auto KeyDetailFlow::fingerprint() const -> const QString & {
    return m_fingerprint;
}

void KeyDetailFlow::showHeader(const QString &title) {
    auto current = keyInfo(m_module, m_fingerprint);
    if (!current.has_value()) {
        return;
    }
    m_title = title;
    auto *old = m_header;
    m_header = keyHeader(title, m_fingerprint, current.value());
    m_slots->setHeader(m_header);
    delete old;
}

// The status on the right follows the key and its device.
void KeyDetailFlow::onKeyChanged() {
    if (m_header != nullptr) {
        showHeader(m_title);
    }
}

} // namespace keys::modal
