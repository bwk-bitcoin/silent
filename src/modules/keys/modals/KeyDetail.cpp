#include "KeyDetail.h"

#include "FetchXpubStep.h"

#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"
#include "modules/keys/flow/PasswordStep.h"

#include "catalog/Button.h"
#include "catalog/Table.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/Section.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/display/ShortValue.h"
#include "catalog/display/WordGrid.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
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

#include <utility>

#include <common.h>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

const char *const K_ROUTE_INDEX = "routeIndex";

// The route kinds a key can be reached over, in the order the picker offers them.
const QList<KeyRouteKind> ROUTE_KINDS = {KeyRouteKind::Hot, KeyRouteKind::Usb, KeyRouteKind::Qr,
                                         KeyRouteKind::SdCard};

auto emptyLine(const QString &text) -> Label * {
    auto *label = new Label(text, LabelRole::Body);
    label->setTone(LabelTone::Muted);
    label->setWordWrap(true);
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

} // namespace

KeyDetail::KeyDetail(KeysModule *module, const QString &fingerprint)
    : m_module(module),
      m_fingerprint(fingerprint) {
    init();
    doConnect();
    view();
    refresh();
    showDetail();
}

KeyDetail::~KeyDetail() {
    m_words->setWords({});
}

void KeyDetail::init() {
    auto current = info();
    m_alias = current.has_value() ? KeysModule::toQt(current->alias) : QString();
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

    m_reveal = catalog::btnRevealMnemonic();
    m_detail_actions = new catalog::ModalActions;
    m_detail_actions->addAction(m_reveal);

    m_fetch = new FetchXpubStep;
    m_fetching = new catalog::ModalStatus;
    m_fetching->setSpinner();
    m_fetching->setTitle(TR("keys-fetching-title"));
    m_fetching->setPadding(resolve(Spacing::M), resolve(Spacing::M));

    m_password = new flow::PasswordStep(catalog::btnShowMnemonic());
    m_password->setBackVisible(true);

    m_words = new catalog::WordGrid;
    m_hide_words = catalog::btnHideWords();
    m_show_words = catalog::btnShowWords();
    m_reveal_actions = new catalog::ModalActions;

    m_remove_question = new Label(LabelRole::Body);
    m_remove_question->setWordWrap(true);
    m_remove_accounts = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_remove_hot = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_remove_hot->setText(TR("keys-remove-hot"));
    m_remove_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_remove_actions = new catalog::ModalActions;
    m_remove_actions->setBack(catalog::ModalActions::Back::Back);
}

void KeyDetail::doConnect() {
    connect(m_module, &KeysModule::keysChanged, this, &KeyDetail::refresh, qontrol::UNIQUE);
    connect(m_module, &KeysModule::liveDevicesChanged, this, &KeyDetail::refresh, qontrol::UNIQUE);
    connect(m_module, &KeysModule::mnemonicRevealed, this, &KeyDetail::onMnemonicRevealed,
            qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &KeyDetail::onInstanceRemoved, qontrol::UNIQUE);
    connect(m_alias_input, &QLineEdit::textEdited, this, &KeyDetail::onAliasEdited,
            qontrol::UNIQUE);
    connect(m_alias_input, &QLineEdit::returnPressed, this, &KeyDetail::saveAlias, qontrol::UNIQUE);
    connect(m_save_alias, &QPushButton::clicked, this, &KeyDetail::saveAlias, qontrol::UNIQUE);
    for (auto *table : {m_devices_table, m_routes_table, m_xpubs_table, m_accounts_table}) {
        connect(table, &catalog::Table::rebuilt, this, &KeyDetail::refit, qontrol::UNIQUE);
    }
    connect(m_reveal, &QPushButton::clicked, this, &KeyDetail::showPassword, qontrol::UNIQUE);
    connect(m_fetch, &FetchXpubStep::fetchClicked, this, &KeyDetail::onFetchClicked,
            qontrol::UNIQUE);
    connect(m_fetch, &FetchXpubStep::backClicked, this, &KeyDetail::showDetail, qontrol::UNIQUE);
    connect(m_password, &flow::PasswordStep::passwordEntered, this, &KeyDetail::onPasswordEntered,
            qontrol::UNIQUE);
    connect(m_password, &flow::PasswordStep::backClicked, this, &KeyDetail::showDetail,
            qontrol::UNIQUE);
    connect(m_hide_words, &QPushButton::clicked, this, &KeyDetail::toggleWords, qontrol::UNIQUE);
    connect(m_show_words, &QPushButton::clicked, this, &KeyDetail::toggleWords, qontrol::UNIQUE);
    connect(m_remove_actions, &catalog::ModalActions::backClicked, this, &KeyDetail::showDetail,
            qontrol::UNIQUE);
}

void KeyDetail::view() {
    auto *removeKey = catalog::btnRemoveKey();
    connect(removeKey, &QPushButton::clicked, this, &KeyDetail::showRemove, qontrol::UNIQUE);
    m_detail_actions->addAction(removeKey);
    m_detail_page = addStep(m_alias, ModalWidth::L, buildDetail(), m_detail_actions);

    m_fetch_page = addStep(TR("keys-fetch-title"), ModalWidth::L, m_fetch, m_fetch->actions());
    m_fetching_page = addStep(TR("keys-fetch-title"), ModalWidth::L, m_fetching, nullptr);
    m_password_page =
        addStep(TR("keys-reveal-title"), ModalWidth::L, m_password, m_password->actions());

    auto *done = catalog::btnDone();
    connect(done, &QPushButton::clicked, this, &KeyDetail::closeWords, qontrol::UNIQUE);
    m_reveal_actions->addAction(done);
    m_reveal_page =
        addStep(TR("keys-reveal-title"), ModalWidth::L, buildReveal(), m_reveal_actions);

    auto *confirmRemove = catalog::btnRemoveKey();
    connect(confirmRemove, &QPushButton::clicked, this, &KeyDetail::removeKey, qontrol::UNIQUE);
    m_remove_actions->addAction(confirmRemove);
    m_remove_page =
        addStep(TR("keys-remove-title"), ModalWidth::L, buildRemove(), m_remove_actions);
}

auto KeyDetail::buildDetail() -> QWidget * {
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
    m_accounts_section->setContent(
        (new qontrol::Column)->push(m_accounts_table)->push(m_accounts_empty));
    for (auto *section :
         {m_devices_section, m_routes_section, m_xpubs_section, m_accounts_section}) {
        connect(section->header(), &catalog::SectionHeader::toggled, this, &KeyDetail::refit,
                qontrol::UNIQUE);
    }

    return (new qontrol::Column)
        ->spacing(resolve(Spacing::XS))
        ->push(alias)
        ->push(m_route_error)
        ->push(m_devices_section)
        ->push(m_routes_section)
        ->push(m_xpubs_section)
        ->push(m_accounts_section);
}

auto KeyDetail::buildReveal() -> QWidget * {
    auto *warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    warning->setTitle(TR("keys-reveal-warning-title"));
    warning->setText(TR("keys-reveal-warning"));
    auto *toggle = (new qontrol::Row)->pushSpacer()->push(m_hide_words)->push(m_show_words);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(warning)
        ->push((new qontrol::Column)->spacing(resolve(Spacing::S))->push(toggle)->push(m_words));
}

auto KeyDetail::buildRemove() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_remove_question)
        ->push(m_remove_accounts)
        ->push(m_remove_hot)
        ->push(m_remove_error);
}

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

void KeyDetail::refresh() {
    auto current = info();
    if (!current.has_value()) {
        // Removed: the window is closing.
        return;
    }
    m_alias = KeysModule::toQt(current->alias);
    setStepTitle(m_detail_page, m_alias);
    onAliasEdited();
    refreshHeader(current.value());
    refreshDevices();
    refreshRoutes(current.value());
    refreshXpubs();
    refreshAccounts();
    m_reveal->setVisible(current->has_hot_secret);
}

void KeyDetail::refit() {
    showStep(currentStep());
}

void KeyDetail::refreshHeader(const KeyInfo &info) {
    auto *fingerprint = new Label(m_fingerprint, LabelRole::Mono);
    fingerprint->setTone(LabelTone::Secondary);
    fingerprint->setSize(catalog::LabelSize::Sm);
    setHeaderEnd((new qontrol::Row)
                     ->spacing(resolve(Spacing::S))
                     ->push(fingerprint)
                     ->push(keyStatusPill(info)));
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
        connect(cancel, &QPushButton::clicked, this, &KeyDetail::cancelAddingRoute,
                qontrol::UNIQUE);
        auto *confirm = catalog::btnConfirmAddRoute();
        connect(confirm, &QPushButton::clicked, this, &KeyDetail::confirmAddRoute, qontrol::UNIQUE);
        actions->push(cancel)->push(confirm);
    } else {
        auto *add = catalog::btnAddRouteIcon();
        connect(add, &QPushButton::clicked, this, &KeyDetail::startAddingRoute, qontrol::UNIQUE);
        actions->push(add);
    }
    m_routes_table->addWidgetRow(QStringLiteral("add"), {nullptr, m_route_picker, actions},
                                 !m_adding_route);
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
    connect(fetch, &QPushButton::clicked, this, &KeyDetail::showFetch, qontrol::UNIQUE);
    m_xpubs_table->addWidgetRow(QStringLiteral("add"), {nullptr, empty, fetch}, true);
    m_xpubs_section->header()->setCount(countText(static_cast<int>(xpubs.size())));
}

void KeyDetail::refreshAccounts() {
    QStringList names = accountNames();
    m_accounts_table->clearRows();
    for (const auto &name : names) {
        auto *module = new Label(TR("keys-accounts-module-sp"), LabelRole::Body);
        module->setTone(LabelTone::Muted);
        m_accounts_table->addWidgetRow(
            name, {new Label(name, LabelRole::Body), module, catalog::pillAccountRegistered()});
    }
    m_accounts_table->setVisible(!names.isEmpty());
    m_accounts_empty->setVisible(names.isEmpty());
    m_accounts_section->header()->setCount(countText(static_cast<int>(names.size())));
}

auto KeyDetail::accountNames() const -> QStringList {
    // Accounts are told apart by the descriptor they were created with.
    QHash<QString, QString> accounts;
    for (const auto &name : ::list_configs()) {
        auto config = ::config_from_file(name);
        if (config->is_ok()) {
            accounts.insert(KeysModule::toQt(config->get_descriptor()), KeysModule::toQt(name));
        }
    }
    QStringList names;
    for (const auto &descriptor :
         m_module->rust()->key_descriptors(rust::String(m_fingerprint.toStdString()))) {
        names.append(accounts.value(KeysModule::toQt(descriptor.descriptor),
                                    KeysModule::toQt(descriptor.alias)));
    }
    return names;
}

void KeyDetail::onAliasEdited() {
    bool changed = m_alias_input->text().trimmed() != m_alias;
    m_save_alias->setVisible(changed);
    if (changed && m_alias_note->isVisible()) {
        m_alias_note->setVisible(false);
        refit();
    }
}

void KeyDetail::saveAlias() {
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
    m_alias_note->setTone(error.isEmpty() ? catalog::FieldNote::Tone::Success
                                          : catalog::FieldNote::Tone::Error);
    m_alias_note->setText(error.isEmpty() ? TR("keys-alias-saved") : error);
    m_alias_note->setVisible(true);
    refit();
}

void KeyDetail::onMoveUpClicked() {
    int index = sender()->property(K_ROUTE_INDEX).toInt();
    moveRoute(index, index - 1);
}

void KeyDetail::onMoveDownClicked() {
    int index = sender()->property(K_ROUTE_INDEX).toInt();
    moveRoute(index, index + 1);
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

void KeyDetail::onRemoveRouteClicked() {
    auto current = info();
    if (!current.has_value()) {
        return;
    }
    auto index = static_cast<std::size_t>(sender()->property(K_ROUTE_INDEX).toInt());
    showRouteResult(m_module->removeRoute(m_fingerprint, current->routes[index]));
}

void KeyDetail::startAddingRoute() {
    m_adding_route = true;
    refresh();
}

void KeyDetail::confirmAddRoute() {
    if (m_route_picker == nullptr) {
        return;
    }
    auto kind = static_cast<KeyRouteKind>(m_route_picker->currentData().toInt());
    m_adding_route = false;
    auto result = m_module->addRoute(m_fingerprint, kind);
    showRouteResult(result);
}

void KeyDetail::cancelAddingRoute() {
    m_adding_route = false;
    refresh();
}

void KeyDetail::showRouteResult(const KeyOpResult &result) {
    m_route_error->setVisible(!result.is_ok);
    if (!result.is_ok) {
        m_route_error->setText(TR("keys-route-failed").arg(KeysModule::toQt(result.error)));
        // A failed change leaves the record as it was: show it as it is.
        refresh();
    }
}

void KeyDetail::showDetail() {
    stopFetch();
    showStep(m_detail_page);
}

void KeyDetail::showFetch() {
    QStringList cached;
    for (const auto &entry :
         m_module->rust()->key_xpubs(rust::String(m_fingerprint.toStdString()))) {
        cached.append(xpubPath(KeysModule::toQt(entry.path)));
    }
    m_fetch->reset(cached);
    showStep(m_fetch_page);
}

void KeyDetail::onFetchClicked(const QString &path) {
    auto device = readyDevice(m_fingerprint);
    if (!device.has_value()) {
        m_fetch->showError(TR("keys-fetch-xpub-connect"));
        refit();
        return;
    }
    m_fetch_signer = device->signer;
    m_fetch_instance = device->instanceId;
    m_fetch_path = path;
    connect(device->signer, &ISigner::xpub, this, &KeyDetail::onDeviceXpub, qontrol::UNIQUE);
    connect(device->signer, &ISigner::error, this, &KeyDetail::onDeviceError, qontrol::UNIQUE);
    m_fetch_req = device->signer->getXpub(path);
    m_fetching->setSubtitle(TR("keys-fetching-sub").arg(deviceName(*device->signer)));
    showStep(m_fetching_page);
}

void KeyDetail::onDeviceXpub(ReqId req_id, const QString &xpub) {
    if (m_fetch_req == 0 || req_id != m_fetch_req) {
        return;
    }
    QString instance = m_fetch_instance;
    QString path = m_fetch_path;
    stopFetch();
    auto result = m_module->cacheXpub(m_fingerprint, path, xpub);
    if (!result.is_ok) {
        showStep(m_fetch_page);
        m_fetch->showError(KeysModule::toQt(result.error));
        refit();
        return;
    }
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.instanceId == instance) {
            auto recorded = m_module->recordDevice(entry);
            if (!recorded.is_ok) {
                qWarning() << "keys:" << KeysModule::toQt(recorded.error);
            }
        }
    }
    showDetail();
}

void KeyDetail::onDeviceError(std::optional<ReqId> req_id, const QString &message) {
    if (m_fetch_req == 0 || !req_id.has_value() || req_id.value() != m_fetch_req) {
        return;
    }
    stopFetch();
    showStep(m_fetch_page);
    m_fetch->showError(message);
    refit();
}

void KeyDetail::onInstanceRemoved(const QString &id) {
    if (m_fetch_req == 0 || id != m_fetch_instance) {
        return;
    }
    stopFetch();
    showStep(m_fetch_page);
    m_fetch->showError(TR("keys-register-disconnected"));
    refit();
}

void KeyDetail::stopFetch() {
    if (!m_fetch_signer.isNull()) {
        disconnect(m_fetch_signer, nullptr, this, nullptr);
    }
    m_fetch_signer = nullptr;
    m_fetch_req = 0;
}

void KeyDetail::showPassword() {
    m_password->reset();
    showStep(m_password_page);
}

void KeyDetail::onPasswordEntered(QString password) {
    m_password->setBusy(true);
    m_reveal_req = m_module->nextReqId();
    m_module->revealMnemonic(m_reveal_req, m_fingerprint, password);
    password.fill(QChar());
    password.clear();
}

void KeyDetail::onMnemonicRevealed(ReqId req_id, const MnemonicResult &result) {
    if (m_reveal_req == 0 || req_id != m_reveal_req) {
        return;
    }
    m_reveal_req = 0;
    m_password->setBusy(false);
    if (!result.is_ok) {
        m_password->reset();
        m_password->showError(result.wrong_password
                                  ? TR("flow-password-wrong")
                                  : TR("keys-reveal-failed").arg(KeysModule::toQt(result.error)));
        refit();
        return;
    }
    // A QString cannot be zeroized: Rust owns the real wiping of the phrase.
    QString mnemonic = KeysModule::toQt(result.mnemonic);
    m_words->setWords(mnemonic.split(QLatin1Char(' '), Qt::SkipEmptyParts));
    mnemonic.fill(QChar());
    m_words->setMasked(false);
    m_hide_words->setVisible(true);
    m_show_words->setVisible(false);
    showStep(m_reveal_page);
}

void KeyDetail::toggleWords() {
    bool masked = m_hide_words->isVisible();
    m_words->setMasked(masked);
    m_hide_words->setVisible(!masked);
    m_show_words->setVisible(masked);
}

void KeyDetail::closeWords() {
    m_words->setWords({});
    showDetail();
}

void KeyDetail::showRemove() {
    auto current = info();
    if (!current.has_value()) {
        return;
    }
    m_remove_question->setText(TR("keys-remove-question").arg(m_alias, m_fingerprint));
    QStringList names = accountNames();
    m_remove_accounts->setTitle(names.size() == 1
                                    ? TR("keys-remove-accounts-one")
                                    : TR("keys-remove-accounts-many").arg(names.size()));
    m_remove_accounts->setText(
        TR("keys-remove-accounts-body").arg(names.join(QStringLiteral(", "))));
    m_remove_accounts->setVisible(!names.isEmpty());
    m_remove_hot->setVisible(current->has_hot_secret);
    m_remove_error->setVisible(false);
    showStep(m_remove_page);
}

void KeyDetail::removeKey() {
    bool registered =
        !m_module->rust()->key_descriptors(rust::String(m_fingerprint.toStdString())).empty();
    auto result = m_module->removeKey(m_fingerprint, registered);
    if (!result.is_ok) {
        m_remove_error->setText(TR("keys-remove-failed").arg(KeysModule::toQt(result.error)));
        m_remove_error->setVisible(true);
        refit();
        return;
    }
    accept();
}

} // namespace keys::modal
