#include "KeyDetail.h"

#include "FetchXpub.h"

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
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/display/ShortValue.h"
#include "catalog/display/WordGrid.h"
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

KeyDetail::~KeyDetail() {
    m_password_input->clear();
    m_words->setWords({});
}

void KeyDetail::onBackClicked() {
    switch (m_view) {
    case View::Password:
    case View::Remove:
        showDetail();
        return;
    case View::Detail:
    case View::Reveal:
        return;
    }
}

void KeyDetail::onNextClicked() {
    switch (m_view) {
    case View::Password:
        proceedPassword();
        return;
    case View::Reveal:
        closeWords();
        return;
    case View::Remove:
        removeKey();
        return;
    case View::Detail:
        return;
    }
}

void KeyDetail::onKeyChanged() {
    auto current = info();
    if (!current.has_value()) {
        // Removed: the window is closing.
        return;
    }
    m_alias = KeysModule::toQt(current->alias);
    // The detail's title is the alias, which may have just changed.
    showHeader(m_detail->isVisibleTo(this) ? m_alias : windowTitle());
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
    auto current = info();
    if (!current.has_value()) {
        return;
    }
    auto index = static_cast<std::size_t>(sender()->property(K_ROUTE_INDEX).toInt());
    showRouteResult(m_module->removeRoute(m_fingerprint, current->routes[index]));
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

void KeyDetail::onPasswordEdited() {
    bool empty = m_password_input->text().isEmpty();
    if (!empty) {
        m_password_error->setVisible(false);
    }
    m_password_proceed->setEnabled(!empty);
    m_password_actions->setReason(empty ? TR("flow-password-reason") : QString());
}

void KeyDetail::onMnemonicRevealed(ReqId req_id, const MnemonicResult &result) {
    if (m_reveal_req == 0 || req_id != m_reveal_req) {
        return;
    }
    m_reveal_req = 0;
    if (!result.is_ok) {
        m_password_error->setText(
            result.wrong_password ? TR("flow-password-wrong")
                                  : TR("keys-reveal-failed").arg(KeysModule::toQt(result.error)));
        m_password_error->setVisible(true);
        showPassword();
        return;
    }
    // A QString cannot be zeroized: Rust owns the real wiping of the phrase.
    QString mnemonic = KeysModule::toQt(result.mnemonic);
    m_words->setWords(mnemonic.split(QLatin1Char(' '), Qt::SkipEmptyParts));
    mnemonic.fill(QChar());
    showReveal();
}

void KeyDetail::onToggleWordsClicked() {
    bool masked = m_hide_words->isVisible();
    m_words->setMasked(masked);
    m_hide_words->setVisible(!masked);
    m_show_words->setVisible(masked);
}

void KeyDetail::init() {
    auto current = info();
    m_alias = current.has_value() ? KeysModule::toQt(current->alias) : QString();
    initDetail();
    initPassword();
    initReveal();
    initRemove();
}

void KeyDetail::doConnect() {
    connect(m_module, &KeysModule::keysChanged, this, &KeyDetail::onKeyChanged, qontrol::UNIQUE);
    connect(m_module, &KeysModule::liveDevicesChanged, this, &KeyDetail::onKeyChanged,
            qontrol::UNIQUE);
    connectDetail();
    connectPassword();
    connectReveal();
    connectRemove();
}

void KeyDetail::view() {
    m_detail = viewDetail();
    m_password_body = viewPassword();
    m_reveal_body = viewReveal();
    m_remove_body = viewRemove();

    // Owned by the modal, hidden until their view shows them.
    QList<QWidget *> views = {m_password_body,  m_password_actions, m_reveal_body,
                              m_reveal_actions, m_remove_body,      m_remove_actions};
    for (auto *widget : views) {
        widget->hide();
        widget->setParent(this);
    }
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
    connect(m_reveal, &QPushButton::clicked, this, &KeyDetail::showPassword, qontrol::UNIQUE);
    connect(m_remove, &QPushButton::clicked, this, &KeyDetail::showRemove, qontrol::UNIQUE);
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
    m_view = View::Detail;
    // A password error is about the attempt left behind.
    m_password_error->setVisible(false);
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
    auto current = info();
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
        m_xpubs_table->addWidgetRow(xpub,
                                    {path, new catalog::ShortValue(xpub),
                                     used ? catalog::pillXpubUsed() : catalog::pillXpubUnused()});
    }

    auto device = readyDevice(m_fingerprint);
    Label *empty = nullptr;
    if (xpubs.empty()) {
        empty =
            emptyLine(device.has_value() ? TR("keys-xpubs-empty") : TR("keys-xpubs-empty-offline"));
    }
    auto *fetch = catalog::btnFetchXpub();
    fetch->setEnabled(device.has_value());
    fetch->setToolTip(device.has_value()
                          ? TR("keys-fetch-xpub-from").arg(deviceName(*device->signer))
                          : TR("keys-fetch-xpub-connect"));
    connect(fetch, &QPushButton::clicked, this, &KeyDetail::onFetchXpubClicked, qontrol::UNIQUE);
    m_xpubs_table->addWidgetRow(QStringLiteral("add"), {nullptr, empty, fetch}, true);
    m_xpubs_section->header()->setCount(countText(static_cast<int>(xpubs.size())));
}

void KeyDetail::refreshAccounts(const KeyInfo &info) {
    QList<KeyAccount> list = accounts();
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

auto KeyDetail::accounts() const -> QList<KeyAccount> {
    // Accounts are told apart by the descriptor they were created with.
    QHash<QString, QString> names;
    for (const auto &name : ::list_configs()) {
        auto config = ::config_from_file(name);
        if (config->is_ok()) {
            names.insert(KeysModule::toQt(config->get_descriptor()), KeysModule::toQt(name));
        }
    }
    QList<KeyAccount> list;
    for (const auto &descriptor :
         m_module->rust()->key_descriptors(rust::String(m_fingerprint.toStdString()))) {
        QString value = KeysModule::toQt(descriptor.descriptor);
        list.append(
            {.name = names.value(value, KeysModule::toQt(descriptor.alias)), .descriptor = value});
    }
    return list;
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

// ===== Password view =====

void KeyDetail::initPassword() {
    m_password_input = new catalog::Input;
    m_password_input->setEchoMode(QLineEdit::Password);
    m_password_input->setFixedWidth(metric::PASSWORD_INPUT_WIDTH);
    m_password_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_password_error->setVisible(false);
    m_password_proceed = catalog::btnShowMnemonic();
    m_password_unlocking = catalog::btnUnlocking();
    m_password_unlocking->setEnabled(false);
    m_password_unlocking->setVisible(false);
    m_password_actions = new catalog::ModalActions;
    m_password_actions->setBack(catalog::ModalActions::Back::Back);
    m_password_actions->addAction(m_password_unlocking);
    m_password_actions->addAction(m_password_proceed);
}

void KeyDetail::connectPassword() {
    connect(m_password_input, &QLineEdit::textEdited, this, &KeyDetail::onPasswordEdited,
            qontrol::UNIQUE);
    connect(m_password_input, &QLineEdit::returnPressed, this, &KeyDetail::onNextClicked,
            qontrol::UNIQUE);
    connect(m_password_proceed, &QPushButton::clicked, this, &KeyDetail::onNextClicked,
            qontrol::UNIQUE);
    connect(m_password_actions, &catalog::ModalActions::backClicked, this,
            &KeyDetail::onBackClicked, qontrol::UNIQUE);
    connect(m_module, &KeysModule::mnemonicRevealed, this, &KeyDetail::onMnemonicRevealed,
            qontrol::UNIQUE);
}

auto KeyDetail::viewPassword() -> QWidget * {
    auto *body = new QWidget;
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(new catalog::Field(TR("flow-password"), m_password_input))
        ->push(m_password_error)
        ->into(body);
    return body;
}

// An empty field with the focus in it, and the error kept if one was just shown.
void KeyDetail::showPassword() {
    m_view = View::Password;
    m_password_input->clear();
    m_password_input->setEnabled(true);
    m_password_unlocking->setVisible(false);
    m_password_proceed->setVisible(true);
    onPasswordEdited();
    showHeader(TR("keys-reveal-title"));
    setBody(m_password_body);
    setFooter(m_password_actions);
    setWidth(ModalWidth::L);
    m_password_input->setFocus();
}

void KeyDetail::proceedPassword() {
    if (m_reveal_req != 0 || m_password_input->text().isEmpty()) {
        return;
    }
    QString password = m_password_input->text();
    m_password_input->clear();
    m_password_input->setEnabled(false);
    m_password_error->setVisible(false);
    m_password_proceed->setVisible(false);
    m_password_unlocking->setVisible(true);
    m_password_actions->setReason(QString());
    m_reveal_req = m_module->nextReqId();
    m_module->revealMnemonic(m_reveal_req, m_fingerprint, password);
    password.fill(QChar());
    password.clear();
}

// ===== Reveal view =====

void KeyDetail::initReveal() {
    m_words = new catalog::WordGrid;
    m_hide_words = catalog::btnHideWords();
    m_show_words = catalog::btnShowWords();
    m_words_done = catalog::btnDone();
    m_reveal_actions = new catalog::ModalActions;
    m_reveal_actions->addAction(m_words_done);
}

void KeyDetail::connectReveal() {
    connect(m_hide_words, &QPushButton::clicked, this, &KeyDetail::onToggleWordsClicked,
            qontrol::UNIQUE);
    connect(m_show_words, &QPushButton::clicked, this, &KeyDetail::onToggleWordsClicked,
            qontrol::UNIQUE);
    connect(m_words_done, &QPushButton::clicked, this, &KeyDetail::onNextClicked, qontrol::UNIQUE);
}

auto KeyDetail::viewReveal() -> QWidget * {
    auto *warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    warning->setTitle(TR("keys-reveal-warning-title"));
    warning->setText(TR("keys-reveal-warning"));
    auto *toggle = (new qontrol::Row)->pushSpacer()->push(m_hide_words)->push(m_show_words);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(warning)
        ->push((new qontrol::Column)->spacing(resolve(Spacing::S))->push(toggle)->push(m_words));
}

void KeyDetail::showReveal() {
    m_view = View::Reveal;
    m_words->setMasked(false);
    m_hide_words->setVisible(true);
    m_show_words->setVisible(false);
    showHeader(TR("keys-reveal-title"));
    setBody(m_reveal_body);
    setFooter(m_reveal_actions);
    setWidth(ModalWidth::L);
}

void KeyDetail::closeWords() {
    m_words->setWords({});
    showDetail();
}

// ===== Remove view =====

void KeyDetail::initRemove() {
    m_remove_question = new Label(LabelRole::Body);
    m_remove_question->setWordWrap(true);
    m_remove_accounts = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_remove_hot = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_remove_hot->setText(TR("keys-remove-hot"));
    m_remove_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_remove_confirm = catalog::btnRemoveKey();
    m_remove_actions = new catalog::ModalActions;
    m_remove_actions->setBack(catalog::ModalActions::Back::Back);
    m_remove_actions->addAction(m_remove_confirm);
}

void KeyDetail::connectRemove() {
    connect(m_remove_actions, &catalog::ModalActions::backClicked, this, &KeyDetail::onBackClicked,
            qontrol::UNIQUE);
    connect(m_remove_confirm, &QPushButton::clicked, this, &KeyDetail::onNextClicked,
            qontrol::UNIQUE);
}

auto KeyDetail::viewRemove() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_remove_question)
        ->push(m_remove_accounts)
        ->push(m_remove_hot)
        ->push(m_remove_error);
}

void KeyDetail::showRemove() {
    auto current = info();
    if (!current.has_value()) {
        return;
    }
    m_remove_question->setText(TR("keys-remove-question").arg(m_alias, m_fingerprint));
    QStringList names;
    for (const auto &account : accounts()) {
        names.append(account.name);
    }
    m_remove_accounts->setTitle(names.size() == 1
                                    ? TR("keys-remove-accounts-one")
                                    : TR("keys-remove-accounts-many").arg(names.size()));
    m_remove_accounts->setText(
        TR("keys-remove-accounts-body").arg(names.join(QStringLiteral(", "))));
    m_remove_accounts->setVisible(!names.isEmpty());
    m_remove_hot->setVisible(current->has_hot_secret);
    m_remove_error->setVisible(false);
    m_view = View::Remove;
    showHeader(TR("keys-remove-title"));
    setBody(m_remove_body);
    setFooter(m_remove_actions);
    setWidth(ModalWidth::L);
}

void KeyDetail::removeKey() {
    bool registered =
        !m_module->rust()->key_descriptors(rust::String(m_fingerprint.toStdString())).empty();
    auto result = m_module->removeKey(m_fingerprint, registered);
    if (!result.is_ok) {
        m_remove_error->setText(TR("keys-remove-failed").arg(KeysModule::toQt(result.error)));
        m_remove_error->setVisible(true);
        adjustSize();
        return;
    }
    accept();
}

// ===== Shared =====

auto KeyDetail::info() const -> std::optional<KeyInfo> {
    if (m_module->rust() == nullptr) {
        return std::nullopt;
    }
    auto result = m_module->rust()->key(rust::String(m_fingerprint.toStdString()));
    if (!result.is_ok) {
        return std::nullopt;
    }
    return result.key;
}

void KeyDetail::showHeader(const QString &title) {
    auto current = info();
    if (!current.has_value()) {
        return;
    }
    setWindowTitle(title);
    auto *fingerprint = new Label(m_fingerprint, LabelRole::Mono);
    fingerprint->setTone(LabelTone::Secondary);
    fingerprint->setSize(catalog::LabelSize::Sm);
    auto *end = (new qontrol::Row)
                    ->spacing(resolve(Spacing::S))
                    ->push(fingerprint)
                    ->push(keyStatusPill(current.value()));
    auto *old = m_header;
    m_header = catalog::modalHeader(title, end);
    setHeader(m_header);
    delete old;
}

} // namespace keys::modal
