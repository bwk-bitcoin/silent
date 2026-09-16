#include "KeyModal.h"

#include "PasswordPrompt.h"

#include "modules/keys/KeysModule.h"
#include "modules/keys/RouteDisplay.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/form/FormRow.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <QLabel>
#include <optional>
#include <utility>

#include <common.h>

using catalog::Badge;
using catalog::Button;
using catalog::ButtonRole;
using catalog::ComboBox;
using catalog::CopyButton;
using catalog::FormRow;
using catalog::Input;
using catalog::Label;
using catalog::LabelRole;

namespace keys::modal {

namespace {

const char *const K_ROUTE_INDEX = "routeIndex";

// Every read here is guarded the same way: no manager, no data, never a crash.
auto fetchKeyInfo(KeysModule *module, const QString &fingerprint) -> std::optional<KeyInfo> {
    if (module == nullptr || module->rust() == nullptr) {
        return std::nullopt;
    }
    auto result = module->rust()->key(rust::String(fingerprint.toStdString()));
    if (!result.is_ok) {
        return std::nullopt;
    }
    return result.key;
}

auto countRegisteredDescriptors(KeysModule *module, const QString &fingerprint) -> int {
    if (module == nullptr || module->rust() == nullptr) {
        return 0;
    }
    int count = 0;
    for (const auto &entry :
         module->rust()->key_descriptors(rust::String(fingerprint.toStdString()))) {
        if (entry.registered) {
            ++count;
        }
    }
    return count;
}

} // namespace

RemoveKeyConfirm::RemoveKeyConfirm(QWidget *parent) {
    Q_UNUSED(parent);
    setWindowTitle(TR("keys-remove-key"));
    init();
    doConnect();
    view();
}

void RemoveKeyConfirm::init() {
    m_label = new Label(TR("keys-remove-warning"));
    m_label->setWordWrap(true);
    m_cancel_btn = new Button(TR("common-cancel"));
    m_remove_btn = new Button(TR("keys-remove-key"), ButtonRole::Destructive);
}

void RemoveKeyConfirm::doConnect() {
    connect(m_cancel_btn, &QPushButton::clicked, this, &QDialog::reject, qontrol::UNIQUE);
    connect(m_remove_btn, &QPushButton::clicked, this, &RemoveKeyConfirm::onRemoveClicked,
            qontrol::UNIQUE);
}

void RemoveKeyConfirm::onRemoveClicked() {
    emit confirmed();
    accept();
}

void RemoveKeyConfirm::view() {
    auto *buttonRow = (new qontrol::Row)
                          ->pushSpacer()
                          ->push(m_cancel_btn)
                          ->pushSpacer(resolve(Spacing::XS))
                          ->push(m_remove_btn)
                          ->pushSpacer();

    auto *col = (new qontrol::Column)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(m_label)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(buttonRow)
                    ->pushSpacer();

    setMainWidget(margin(col));
}

KeyModal::KeyModal(KeysModule *module, QString fingerprint, QWidget *parent)
    : m_module(module),
      m_fingerprint(std::move(fingerprint)) {
    Q_UNUSED(parent);
    setWindowTitle(TR("keys-key-title"));
    init();
    doConnect();
    view();
}

KeyModal::~KeyModal() {
    clearRevealedMnemonic();
}

void KeyModal::init() {
    auto info = fetchKeyInfo(m_module, m_fingerprint);
    QString alias = info.has_value() ? KeysModule::toQt(info->alias) : QString();

    m_alias_input = new Input(alias);
    m_alias_input->setWidth(Size::M);
    m_save_alias_btn = new Button(TR("common-apply"));

    m_route_picker = new ComboBox;
    m_route_picker->addItem(TR("keys-route-hot"), static_cast<int>(KeyRouteKind::Hot));
    m_route_picker->addItem(TR("keys-route-usb"), static_cast<int>(KeyRouteKind::Usb));
    m_route_picker->addItem(TR("keys-route-qr"), static_cast<int>(KeyRouteKind::Qr));
    m_route_picker->addItem(TR("keys-route-sd"), static_cast<int>(KeyRouteKind::SdCard));

    m_add_route_btn = new Button(TR("keys-add-route"));
    m_reveal_btn = new Button(TR("keys-reveal"));
    m_reveal_btn->setVisible(info.has_value() && info->has_hot_secret);
    m_revealed_mnemonic_label = new Label(QString(), LabelRole::Mono);
    m_revealed_mnemonic_label->setWordWrap(true);
    m_revealed_mnemonic_label->setVisible(false);
    m_remove_key_btn = new Button(TR("keys-remove-key"), ButtonRole::Destructive);
    m_close_btn = new Button(TR("common-close"));

    m_remove_hint = new Label(QString(), LabelRole::Caption);
    m_remove_hint->setVisible(false);
    m_error_label = new Label(QString(), LabelRole::Caption);
    m_error_label->setVisible(false);
    if (!info.has_value()) {
        showError(TR("keys-key-load-failed"));
    }

    m_devices_empty = new Label(TR("keys-no-devices-recorded"), LabelRole::Caption);
    m_xpubs_empty = new Label(TR("keys-no-xpubs"), LabelRole::Caption);
    m_descriptors_empty = new Label(TR("keys-no-descriptors"), LabelRole::Caption);

    m_route_column = new qontrol::Column;
    m_device_column = new qontrol::Column;
    m_xpub_column = new qontrol::Column;
    m_descriptor_column = new qontrol::Column;
}

void KeyModal::doConnect() {
    connect(m_save_alias_btn, &QPushButton::clicked, this, &KeyModal::onSaveAliasClicked,
            qontrol::UNIQUE);
    connect(m_add_route_btn, &QPushButton::clicked, this, &KeyModal::onAddRouteClicked,
            qontrol::UNIQUE);
    connect(m_reveal_btn, &QPushButton::clicked, this, &KeyModal::onRevealClicked, qontrol::UNIQUE);
    connect(m_remove_key_btn, &QPushButton::clicked, this, &KeyModal::onRemoveKeyClicked,
            qontrol::UNIQUE);
    connect(m_close_btn, &QPushButton::clicked, this, &QDialog::accept, qontrol::UNIQUE);
    connect(m_module, &KeysModule::mnemonicRevealed, this, &KeyModal::onMnemonicRevealed,
            qontrol::UNIQUE);
}

void KeyModal::view() {
    auto *aliasRow = (new qontrol::Row)
                         ->push(new FormRow(TR("keys-alias"), m_alias_input))
                         ->pushSpacer(resolve(Spacing::XS))
                         ->push(m_save_alias_btn);

    auto *routePickerRow = (new qontrol::Row)
                               ->push(m_route_picker)
                               ->pushSpacer(resolve(Spacing::XS))
                               ->push(m_add_route_btn)
                               ->pushSpacer();

    auto *bottomRow = (new qontrol::Row)
                          ->push(m_remove_key_btn)
                          ->push(m_reveal_btn)
                          ->pushSpacer()
                          ->push(m_close_btn);

    auto *content = (new qontrol::Column)
                        ->push(new Label(m_fingerprint, LabelRole::Mono))
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_error_label)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(aliasRow)
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_revealed_mnemonic_label)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new catalog::Separator)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new Label(TR("keys-routes"), LabelRole::Heading))
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_route_column)
                        ->push(routePickerRow)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new catalog::Separator)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new Label(TR("keys-devices-recorded"), LabelRole::Heading))
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_devices_empty)
                        ->push(m_device_column)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new catalog::Separator)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new Label(TR("keys-xpubs"), LabelRole::Heading))
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_xpubs_empty)
                        ->push(m_xpub_column)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new catalog::Separator)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new Label(TR("keys-descriptors"), LabelRole::Heading))
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_descriptors_empty)
                        ->push(m_descriptor_column)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(new catalog::Separator)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(m_remove_hint)
                        ->push(bottomRow);

    auto *scroll = new catalog::ScrollArea;
    scroll->setWidget(margin(content));
    setMainWidget(scroll);

    rebuildRoutes();
    rebuildDevices();
    rebuildXpubs();
    rebuildDescriptors();
    updateRemoveState();
}

void KeyModal::rebuildRoutes() {
    m_route_column->clear();

    auto info = fetchKeyInfo(m_module, m_fingerprint);
    if (!info.has_value()) {
        return;
    }

    int count = static_cast<int>(info->routes.size());
    for (int index = 0; index < count; ++index) {
        KeyRouteKind kind = info->routes[static_cast<std::size_t>(index)];

        auto *iconLabel = new QLabel;
        iconLabel->setPixmap(
            routeIcon(kind).pixmap(metric::SELECT_ROW_ICON_SIZE, metric::SELECT_ROW_ICON_SIZE));

        auto *upBtn = new Button(ButtonRole::InlineIcon);
        upBtn->setIcon(icon::chevronLeft(IconColor::Accent));
        upBtn->setToolTip(TR("keys-route-up"));
        upBtn->setProperty(K_ROUTE_INDEX, index);
        upBtn->setEnabled(index > 0);
        connect(upBtn, &QPushButton::clicked, this, &KeyModal::onMoveRouteUpClicked,
                qontrol::UNIQUE);

        auto *downBtn = new Button(ButtonRole::InlineIcon);
        downBtn->setIcon(icon::chevronRight(IconColor::Accent));
        downBtn->setToolTip(TR("keys-route-down"));
        downBtn->setProperty(K_ROUTE_INDEX, index);
        downBtn->setEnabled(index < count - 1);
        connect(downBtn, &QPushButton::clicked, this, &KeyModal::onMoveRouteDownClicked,
                qontrol::UNIQUE);

        auto *removeBtn = new Button(ButtonRole::InlineIcon);
        removeBtn->setIcon(icon::trash(IconColor::Error));
        removeBtn->setToolTip(TR("keys-route-remove"));
        removeBtn->setProperty(K_ROUTE_INDEX, index);
        connect(removeBtn, &QPushButton::clicked, this, &KeyModal::onRemoveRouteClicked,
                qontrol::UNIQUE);

        auto *row = (new qontrol::Row)
                        ->push(iconLabel)
                        ->push(new Label(routeLabel(kind), LabelRole::Body))
                        ->pushSpacer();
        if (index == 0) {
            row->push(new Badge(TR("keys-route-primary"), Badge::Role::Success));
        }
        row->push(upBtn)->push(downBtn)->push(removeBtn);

        m_route_column->push(row)->pushSpacer(resolve(Spacing::S));
    }
}

void KeyModal::rebuildDevices() {
    m_device_column->clear();

    rust::Vec<KeyDevice> devices;
    if (m_module != nullptr && m_module->rust() != nullptr) {
        devices = m_module->rust()->key_devices(rust::String(m_fingerprint.toStdString()));
    }
    m_devices_empty->setVisible(devices.empty());

    for (const auto &device : devices) {
        QString summary = QStringLiteral("%1 %2 %3")
                              .arg(KeysModule::toQt(device.brand), KeysModule::toQt(device.model),
                                   KeysModule::toQt(device.serial))
                              .simplified();
        if (summary.isEmpty()) {
            summary = KeysModule::toQt(device.device_id);
        }

        auto *labelCol = (new qontrol::Column)
                             ->push(new Label(summary, LabelRole::Body))
                             ->push(new Label(KeysModule::toQt(device.module), LabelRole::Caption));

        m_device_column->push((new qontrol::Row)->push(labelCol)->pushSpacer())
            ->pushSpacer(resolve(Spacing::S));
    }
}

void KeyModal::rebuildXpubs() {
    m_xpub_column->clear();

    rust::Vec<KeyXpub> xpubs;
    if (m_module != nullptr && m_module->rust() != nullptr) {
        xpubs = m_module->rust()->key_xpubs(rust::String(m_fingerprint.toStdString()));
    }
    m_xpubs_empty->setVisible(xpubs.empty());

    for (const auto &entry : xpubs) {
        QString xpub = KeysModule::toQt(entry.xpub);

        auto *xpubLabel = new Label(xpub, LabelRole::Mono);
        xpubLabel->setWordWrap(true);

        auto *copyBtn = new CopyButton(CopyButton::Size::Small);
        copyBtn->setTextToCopy(xpub);

        auto *labelCol = (new qontrol::Column)
                             ->push(new Label(KeysModule::toQt(entry.path), LabelRole::InfoLabel))
                             ->push(xpubLabel);

        m_xpub_column->push((new qontrol::Row)->push(labelCol)->pushSpacer()->push(copyBtn))
            ->pushSpacer(resolve(Spacing::S));
    }
}

void KeyModal::rebuildDescriptors() {
    m_descriptor_column->clear();

    rust::Vec<KeyDescriptor> descriptors;
    if (m_module != nullptr && m_module->rust() != nullptr) {
        descriptors = m_module->rust()->key_descriptors(rust::String(m_fingerprint.toStdString()));
    }
    m_descriptors_empty->setVisible(descriptors.empty());

    for (const auto &entry : descriptors) {
        QString descriptor = KeysModule::toQt(entry.descriptor);

        auto *descriptorLabel = new Label(descriptor, LabelRole::Mono);
        descriptorLabel->setWordWrap(true);

        Badge *badge = entry.registered
                           ? new Badge(TR("keys-descriptor-registered"), Badge::Role::Success)
                           : new Badge(TR("keys-descriptor-unregistered"), Badge::Role::Neutral);

        auto *copyBtn = new CopyButton(CopyButton::Size::Small);
        copyBtn->setTextToCopy(descriptor);

        m_descriptor_column
            ->push(
                (new qontrol::Row)->push(descriptorLabel)->pushSpacer()->push(badge)->push(copyBtn))
            ->pushSpacer(resolve(Spacing::S));
    }
}

void KeyModal::updateRemoveState() {
    int registeredCount = countRegisteredDescriptors(m_module, m_fingerprint);
    m_remove_hint->setVisible(registeredCount > 0);
    if (registeredCount > 0) {
        m_remove_hint->setText(TR("keys-remove-blocked").arg(registeredCount));
    }
}

void KeyModal::moveRoute(int from_index, int to_index) {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }
    auto info = fetchKeyInfo(m_module, m_fingerprint);
    if (!info.has_value()) {
        return;
    }

    int count = static_cast<int>(info->routes.size());
    if (from_index < 0 || from_index >= count || to_index < 0 || to_index >= count) {
        return;
    }

    rust::Vec<KeyRouteKind> order = info->routes;
    KeyRouteKind moved = order[static_cast<std::size_t>(from_index)];
    order[static_cast<std::size_t>(from_index)] = order[static_cast<std::size_t>(to_index)];
    order[static_cast<std::size_t>(to_index)] = moved;

    auto result = m_module->setRouteOrder(m_fingerprint, std::move(order));
    if (!result.is_ok) {
        showError(TR("keys-route-failed").arg(KeysModule::toQt(result.error)));
        return;
    }
    m_error_label->setVisible(false);
    rebuildRoutes();
}

auto KeyModal::routeAt(int index) const -> KeyRouteKind {
    auto info = fetchKeyInfo(m_module, m_fingerprint);
    if (!info.has_value() || index < 0 || static_cast<std::size_t>(index) >= info->routes.size()) {
        return KeyRouteKind::Unknown;
    }
    return info->routes[static_cast<std::size_t>(index)];
}

void KeyModal::showError(const QString &message) {
    m_error_label->setText(message);
    m_error_label->setVisible(!message.isEmpty());
}

void KeyModal::clearRevealedMnemonic() {
    // A QString cannot be zeroized: fill()+clear() is best effort. Rust owns the
    // real wiping of the mnemonic it handed back.
    m_revealed_mnemonic.fill(QChar());
    m_revealed_mnemonic.clear();
    m_revealed_mnemonic_label->setText(QString());
    m_revealed_mnemonic_label->setVisible(false);
}

void KeyModal::onSaveAliasClicked() {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }
    auto result = m_module->setAlias(m_fingerprint, m_alias_input->text().trimmed());
    if (!result.is_ok) {
        showError(TR("keys-alias-failed").arg(KeysModule::toQt(result.error)));
        return;
    }
    m_error_label->setVisible(false);
}

void KeyModal::onAddRouteClicked() {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }
    auto kind = static_cast<KeyRouteKind>(m_route_picker->currentData().toInt());
    auto result = m_module->addRoute(m_fingerprint, kind);
    if (!result.is_ok) {
        showError(TR("keys-route-failed").arg(KeysModule::toQt(result.error)));
        return;
    }
    m_error_label->setVisible(false);
    rebuildRoutes();
}

void KeyModal::onRemoveRouteClicked() {
    auto *button = qobject_cast<QPushButton *>(sender());
    if (button == nullptr || m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }
    int index = button->property(K_ROUTE_INDEX).toInt();
    KeyRouteKind kind = routeAt(index);
    if (kind == KeyRouteKind::Unknown) {
        return;
    }
    auto result = m_module->removeRoute(m_fingerprint, kind);
    if (!result.is_ok) {
        showError(TR("keys-route-failed").arg(KeysModule::toQt(result.error)));
        return;
    }
    m_error_label->setVisible(false);
    rebuildRoutes();
}

void KeyModal::onMoveRouteUpClicked() {
    auto *button = qobject_cast<QPushButton *>(sender());
    if (button == nullptr) {
        return;
    }
    int index = button->property(K_ROUTE_INDEX).toInt();
    moveRoute(index, index - 1);
}

void KeyModal::onMoveRouteDownClicked() {
    auto *button = qobject_cast<QPushButton *>(sender());
    if (button == nullptr) {
        return;
    }
    int index = button->property(K_ROUTE_INDEX).toInt();
    moveRoute(index, index + 1);
}

void KeyModal::onRemoveKeyClicked() {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }

    if (countRegisteredDescriptors(m_module, m_fingerprint) == 0) {
        auto result = m_module->removeKey(m_fingerprint, false);
        if (!result.is_ok) {
            showError(TR("keys-remove-failed").arg(KeysModule::toQt(result.error)));
            return;
        }
        accept();
        return;
    }

    auto *confirm = new RemoveKeyConfirm;
    connect(confirm, &RemoveKeyConfirm::confirmed, this, &KeyModal::onRemoveKeyConfirmed,
            qontrol::UNIQUE);
    AppController::execModal(confirm);
}

void KeyModal::onRemoveKeyConfirmed() {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }
    auto result = m_module->removeKey(m_fingerprint, true);
    if (!result.is_ok) {
        showError(TR("keys-remove-failed").arg(KeysModule::toQt(result.error)));
        return;
    }
    accept();
}

// NOLINTNEXTLINE(readability-make-member-function-const)
void KeyModal::onRevealClicked() {
    auto *prompt = new PasswordPrompt(PasswordPrompt::Mode::Use, TR("keys-password-use"));
    connect(prompt, &PasswordPrompt::passwordEntered, this, &KeyModal::onRevealPasswordEntered,
            qontrol::UNIQUE);
    AppController::execModal(prompt);
}

void KeyModal::onRevealPasswordEntered(QString password) {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        password.fill(QChar());
        password.clear();
        return;
    }

    m_reveal_btn->setEnabled(false);
    m_reveal_req_id = m_module->nextReqId();
    m_module->revealMnemonic(m_reveal_req_id, m_fingerprint, password);

    password.fill(QChar());
    password.clear();
}

void KeyModal::onMnemonicRevealed(ReqId req_id, MnemonicResult result) {
    if (req_id != m_reveal_req_id) {
        return;
    }
    m_reveal_btn->setEnabled(true);
    if (!result.is_ok) {
        showError(TR("keys-reveal-failed").arg(KeysModule::toQt(result.error)));
        return;
    }
    m_error_label->setVisible(false);
    m_revealed_mnemonic = KeysModule::toQt(result.mnemonic);
    m_revealed_mnemonic_label->setText(m_revealed_mnemonic);
    m_revealed_mnemonic_label->setVisible(true);
}

} // namespace keys::modal
