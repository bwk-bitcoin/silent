#include "account/views/RestoreAccount.h"

#include "account/RequestId.h"
#include "account/Services.h"
#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Section.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "interfaces/signer.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <Qontrol>
#include <common.h>

namespace account::view {

namespace {

// A BIP352 scan path, `m/352'/coin'/account'/1'/0`, as a locked backup names it: its coin
// captured.
const QRegularExpression SCAN_PATH_RE(QStringLiteral(R"(^m/352'/(\d+)'/\d+'/1'/0$)"));
const char *const K_FINGERPRINT = "fingerprint";

} // namespace

RestoreAccount::RestoreAccount(backend::IAccountBackend *target, IModule *module,
                               DeviceName device_name)
    : m_target(target),
      m_module(module),
      m_device_name(device_name) {
    target->setParent(this);
    init();
    doConnect();
    view();
}

RestoreAccount::~RestoreAccount() {
    leaveDecrypt();
    clearKeys();
}

void RestoreAccount::init() {
    initFile();
    initBusy();
    initDecrypt();
    initReview();
    initRestored();
}

void RestoreAccount::doConnect() {
    connectFile();
    connectDecrypt();
    connectReview();
    connectRestored();
    connect(m_target, &backend::IAccountBackend::backupInspected, this,
            &RestoreAccount::onBackupInspected, qontrol::UNIQUE);
    connect(m_target, &backend::IAccountBackend::backupLocked, this,
            &RestoreAccount::onBackupLocked, qontrol::UNIQUE);
    connect(m_target, &backend::IService::error, this, &RestoreAccount::onServiceError,
            qontrol::UNIQUE);
}

void RestoreAccount::view() {
    m_file_view = viewFile();
    viewBusy();
    m_decrypt_view = viewDecrypt();
    m_review_view = viewReview();
    m_restored_view = viewRestored();
    hideBusy();
    hideDecrypt();
    hideReview();
    hideRestored();
    retranslateUi();
    showFile();
}

void RestoreAccount::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void RestoreAccount::retranslateUi() {
    setWindowTitle(TR("restore-title"));
    m_file_text->setText(TR("restore-file-text"));
    m_file_fields->setFieldLabel(m_file_row, TR("restore-file-field"));
    if (m_backup_path.isEmpty()) {
        m_file_label->setText(TR("restore-no-file"));
    }
    m_decrypt_text->setText(TR("restore-device-description"));
    m_keys_text->setText(TR("restore-keys-text"));
    m_options->setTitle(TR("restore-other-options"));
    m_airgap_text->setText(scanKeyOnly() ? TR("restore-scan-key-help") : TR("restore-airgap-help"));
    m_option_fields->setFieldLabel(m_xpub_row, TR("restore-xpub-field"));
    m_option_fields->setFieldLabel(m_import_xpub, TR("restore-xpub-file-field"));
    m_mnemonic_fields->setFieldLabel(m_mnemonic_input, TR("restore-mnemonic-field"));
    m_mnemonic_warning->setText(TR("restore-mnemonic-warning"));
    m_mnemonic_ack->setText(TR("restore-mnemonic-ack"));
    m_review_fields->setFieldLabel(m_name_input, TR("restore-name-field"));
    m_review_fields->setFieldLabel(m_network_combo, TR("restore-network-field"));
}

// ===== File view =====

void RestoreAccount::initFile() {
    m_file_text = new catalog::Label(catalog::LabelRole::Body);
    m_file_text->setTone(catalog::LabelTone::Secondary);
    m_file_text->setWordWrap(true);
    m_file_label = new catalog::Label(catalog::LabelRole::Body);
    m_file_label->setTone(catalog::LabelTone::Secondary);
    m_choose = catalog::btnChooseFile();
    m_file_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_file_error->setVisible(false);
    m_read = catalog::btnReadBackup();
    m_file_actions = new catalog::ModalActions;
    m_file_actions->addAction(m_read);
    m_file_fields = new catalog::FieldGroup;
}

void RestoreAccount::connectFile() {
    connect(m_choose, &QPushButton::clicked, this, &RestoreAccount::onChooseClicked,
            qontrol::UNIQUE);
    connect(m_read, &QPushButton::clicked, this, &RestoreAccount::onNextClicked, qontrol::UNIQUE);
}

auto RestoreAccount::viewFile() -> QWidget * {
    m_file_row =
        (new qontrol::Row)->spacing(resolve(Spacing::S))->push(m_file_label, 1)->push(m_choose);
    m_file_fields->addField(TR("restore-file-field"), m_file_row);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_file_text)
        ->push(m_file_fields)
        ->push(m_file_error);
}

void RestoreAccount::showFile() {
    m_view = View::File;
    bool chosen = !m_backup_path.isEmpty();
    m_read->setEnabled(chosen);
    m_file_actions->setReason(chosen ? QString() : TR("restore-choose-file"));
    setHeader(nullptr);
    setBody(m_file_view);
    setFooter(m_file_actions);
    setWidth(ModalWidth::M);
}

void RestoreAccount::readBackup() {
    if (!m_read->isEnabled() || m_target == nullptr) {
        return;
    }
    m_file_error->setVisible(false);
    showBusy(TR("restore-reading"));
    m_read_req = nextRequestId();
    m_target->inspectBackup(m_read_req, m_backup, {}, {});
}

// ===== Busy view =====

void RestoreAccount::initBusy() {
    m_busy = new catalog::ModalStatus;
    m_busy->setSpinner();
    m_busy->setPadding(resolve(Spacing::L), resolve(Spacing::L));
}

auto RestoreAccount::viewBusy() -> QWidget * {
    return m_busy;
}

void RestoreAccount::showBusy(const QString &title) {
    m_view = View::Busy;
    m_busy->setTitle(title);
    m_busy->setSubtitle(TR("restore-busy-sub"));
    setHeader(nullptr);
    setBody(m_busy);
    setFooter(nullptr);
    setWidth(ModalWidth::M);
}

void RestoreAccount::hideBusy() {
    keep(m_busy);
}

// ===== Decrypt view =====

void RestoreAccount::initDecrypt() {
    m_decrypt_text = new catalog::Label(catalog::LabelRole::Body);
    m_decrypt_text->setTone(catalog::LabelTone::Secondary);
    m_decrypt_text->setWordWrap(true);
    m_device_rows = new qontrol::Column;
    m_keys_text = new catalog::Label(catalog::LabelRole::Body);
    m_keys_text->setTone(catalog::LabelTone::Secondary);
    m_keys_text->setWordWrap(true);
    m_key_rows = new qontrol::Column;
    if (m_module != nullptr) {
        m_keys = Host::get()->keys(m_module);
    }
    m_airgap_text = new catalog::Label(catalog::LabelRole::Body);
    m_airgap_text->setTone(catalog::LabelTone::Secondary);
    m_airgap_text->setWordWrap(true);
    m_paths_label = new catalog::Label(catalog::LabelRole::Mono);
    m_paths_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_xpub_input = new catalog::ValidatedInput(catalog::InputRole::Mono);
    m_paste_xpub = catalog::btnPasteFromClipboard();
    m_xpub_note = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Error);
    m_xpub_note->setVisible(false);
    m_import_xpub = catalog::btnChooseFile();
    m_mnemonic_warning = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_mnemonic_ack = new catalog::Checkbox;
    m_mnemonic_input = new catalog::ValidatedInput;
    m_mnemonic_input->setEnabled(false);
    m_mnemonic_note = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Error);
    m_mnemonic_note->setVisible(false);
    m_option_fields = new catalog::FieldGroup;
    m_mnemonic_fields = new catalog::FieldGroup;
    m_options = new catalog::FoldSection(TR("restore-other-options"));
    m_decrypt_actions = new catalog::ModalActions;
    m_decrypt_actions->setBack(catalog::ModalActions::Back::Back);
}

void RestoreAccount::connectDecrypt() {
    connect(Host::get()->events(), &HostEvents::instanceRegistered, this,
            &RestoreAccount::onInstanceChanged, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &RestoreAccount::onInstanceChanged, qontrol::UNIQUE);
    connect(m_xpub_input->input(), &QLineEdit::textEdited, this, &RestoreAccount::onXpubEdited,
            qontrol::UNIQUE);
    connect(m_paste_xpub, &QPushButton::clicked, this, &RestoreAccount::onPasteXpubClicked,
            qontrol::UNIQUE);
    connect(m_import_xpub, &QPushButton::clicked, this, &RestoreAccount::onImportXpubClicked,
            qontrol::UNIQUE);
    connect(m_mnemonic_ack, &QCheckBox::toggled, this, &RestoreAccount::onMnemonicAckToggled,
            qontrol::UNIQUE);
    connect(m_mnemonic_input->input(), &QLineEdit::textEdited, this,
            &RestoreAccount::onMnemonicEdited, qontrol::UNIQUE);
    connect(m_decrypt_actions, &catalog::ModalActions::backClicked, this,
            &RestoreAccount::onBackClicked, qontrol::UNIQUE);
    if (m_keys != nullptr) {
        connect(m_keys, &IKeys::spDescriptorReady, this, &RestoreAccount::onSpDescriptorReady,
                qontrol::UNIQUE);
        connect(m_keys, &IKeys::error, this, &RestoreAccount::onKeysError, qontrol::UNIQUE);
        connect(m_keys, &IKeys::cancelled, this, &RestoreAccount::onKeysCancelled, qontrol::UNIQUE);
    }
}

auto RestoreAccount::viewDecrypt() -> catalog::ScrollArea * {
    m_xpub_row =
        (new qontrol::Row)->spacing(resolve(Spacing::S))->push(m_xpub_input, 1)->push(m_paste_xpub);
    m_option_fields->addField(TR("restore-xpub-field"), m_xpub_row)
        ->addNote(m_xpub_note)
        ->addField(TR("restore-xpub-file-field"), m_import_xpub);
    m_mnemonic_fields->addField(TR("restore-mnemonic-field"), m_mnemonic_input)
        ->addNote(m_mnemonic_note);
    m_options->setContent((new qontrol::Column)
                              ->spacing(resolve(Spacing::S))
                              ->push(m_airgap_text)
                              ->push(m_paths_label)
                              ->push(m_option_fields)
                              ->push(m_mnemonic_warning)
                              ->push(m_mnemonic_ack)
                              ->push(m_mnemonic_fields));
    m_options->setExpanded(false);
    m_device_rows->spacing(resolve(Spacing::S));
    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    scroll->setWidget((new qontrol::Column)
                          ->spacing(resolve(Spacing::M))
                          ->push(m_decrypt_text)
                          ->push(m_device_rows)
                          ->push(m_keys_text)
                          ->push(m_key_rows)
                          ->push(m_options));
    return scroll;
}

void RestoreAccount::showDecrypt(const QStringList &derivation_paths) {
    m_view = View::Decrypt;
    m_paths = derivation_paths;
    m_xpub_paths.clear();
    for (const auto &path : m_paths) {
        if (!SCAN_PATH_RE.match(path).hasMatch()) {
            m_xpub_paths.append(path);
        }
    }
    m_paths_label->setText(m_paths.join(QStringLiteral("\n")));
    // No xpub opens a backup keyed by a scan key: the help points to the key or its mnemonic,
    // and the xpub field and file are left out.
    m_airgap_text->setText(scanKeyOnly() ? TR("restore-scan-key-help") : TR("restore-airgap-help"));
    m_option_fields->setVisible(!scanKeyOnly());
    if (m_module != nullptr) {
        Host::get()->requestSignerPolling(m_module, true);
    }
    refreshDevices();
    listKeys();
    setHeader(nullptr);
    setBody(m_decrypt_view);
    setFooter(m_decrypt_actions);
    setWidth(ModalWidth::L);
}

void RestoreAccount::hideDecrypt() {
    keep(m_decrypt_view);
    keep(m_decrypt_actions);
}

// Devices are polled only while they are listed.
void RestoreAccount::leaveDecrypt() {
    stopDevices();
    // Their answers carry scan secrets.
    m_keys_tried.clear();
    if (m_module != nullptr) {
        Host::get()->requestSignerPolling(m_module, false);
    }
}

void RestoreAccount::refreshDevices() {
    catalog::clearRows(m_device_rows);
    QList<catalog::SelectRow *> trying;
    QList<catalog::SelectRow *> locked;
    QList<catalog::SelectRow *> failed;
    QSet<QString> live;
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer == nullptr) {
            continue;
        }
        live.insert(entry.instanceId);
        auto *row = new catalog::SelectRow;
        row->setIcon(icon::usb);
        row->setTitle(m_device_name(*entry.signer));
        row->setChevron(false);
        row->setClickable(false);
        if (entry.signer->state() != plugin::SignerState::Ready) {
            auto detail = entry.signer->stateDetail();
            row->setSub(detail.isEmpty() ? TR("restore-device-unlock") : detail);
            locked.append(row);
            continue;
        }
        if (!m_devices.contains(entry.instanceId)) {
            tryDevice(entry.instanceId, entry.signer);
        }
        const auto &device = m_devices.value(entry.instanceId);
        row->setSub(QStringLiteral("#") + device.fingerprint);
        switch (device.status) {
        case DeviceStatus::Trying:
            row->setNote(TR("restore-device-trying"));
            trying.append(row);
            break;
        case DeviceStatus::Failed:
            row->setNote(TR("restore-device-failed"));
            failed.append(row);
            break;
        case DeviceStatus::NoScanKey:
            row->setNote(TR("restore-device-no-scan-key"));
            failed.append(row);
            break;
        }
    }
    // A device gone is tried again when it comes back.
    m_devices.removeIf([&live](QHash<QString, DeviceTry>::iterator device) {
        return !live.contains(device.key());
    });
    if (live.isEmpty()) {
        m_device_rows->push(
            new catalog::EmptyState(icon::usb, TR("state-no-device"), TR("state-no-device-sub")));
    }
    for (const auto &group : {trying, locked, failed}) {
        for (auto *row : group) {
            m_device_rows->push(row);
        }
    }
    refit();
}

void RestoreAccount::tryDevice(const QString &instance_id, ISigner *signer) {
    connect(signer, &ISigner::xpub, this, &RestoreAccount::onDeviceXpub, qontrol::UNIQUE);
    connect(signer, &ISigner::error, this, &RestoreAccount::onDeviceError, qontrol::UNIQUE);
    auto &device = m_devices[instance_id];
    device.signer = signer;
    device.name = m_device_name(*signer);
    device.fingerprint = signer->fingerprint();
    fetchNext(device);
}

void RestoreAccount::fetchNext(DeviceTry &device) {
    if (device.signer.isNull()) {
        return;
    }
    if (device.path < m_xpub_paths.size()) {
        device.xpub_req = device.signer->getXpub(m_xpub_paths.at(device.path));
        return;
    }
    device.xpub_req = 0;
    if (scanKeyOnly()) {
        device.status = DeviceStatus::NoScanKey;
        return;
    }
    if (device.xpubs.isEmpty() || m_target == nullptr) {
        device.status = DeviceStatus::Failed;
        return;
    }
    device.read_req = nextRequestId();
    m_target->inspectBackup(device.read_req, m_backup, device.xpubs, {});
}

auto RestoreAccount::deviceOf(const QObject *signer, ReqId req_id) -> DeviceTry * {
    for (auto &device : m_devices) {
        if (device.signer.data() == signer && device.xpub_req != 0 && device.xpub_req == req_id) {
            return &device;
        }
    }
    return nullptr;
}

auto RestoreAccount::deviceRead(backend::RequestId request_id) -> DeviceTry * {
    for (auto &device : m_devices) {
        if (device.read_req != 0 && device.read_req == request_id) {
            return &device;
        }
    }
    return nullptr;
}

void RestoreAccount::listKeys() {
    m_keys_tried.clear();
    QList<plugin::Network> networks;
    for (const auto &path : m_paths) {
        auto match = SCAN_PATH_RE.match(path);
        if (!match.hasMatch()) {
            continue;
        }
        // Coin 1 is every test network's: a key may be registered on any of them.
        networks = match.captured(1) == QStringLiteral("0")
                       ? QList<plugin::Network>{plugin::Network::Bitcoin}
                       : QList<plugin::Network>{plugin::Network::Testnet, plugin::Network::Signet,
                                                plugin::Network::Regtest};
        break;
    }
    QSet<QString> listed;
    for (auto network : networks) {
        if (m_keys == nullptr) {
            break;
        }
        for (const auto &entry : m_keys->keys(network).entries) {
            if (entry.descriptor_route == plugin::KeyRouteKind::Unknown ||
                listed.contains(entry.fingerprint)) {
                continue;
            }
            listed.insert(entry.fingerprint);
            m_keys_tried.append(
                {.fingerprint = entry.fingerprint, .alias = entry.alias, .network = network});
        }
    }
    refreshKeyRows();
}

void RestoreAccount::refreshKeyRows() {
    catalog::clearRows(m_key_rows);
    m_key_rows->spacing(resolve(Spacing::S));
    m_keys_text->setVisible(!m_keys_tried.isEmpty());
    for (const auto &key : m_keys_tried) {
        auto *row = new catalog::SelectRow;
        row->setIcon(icon::key);
        row->setTitle(key.alias.isEmpty() ? key.fingerprint : key.alias);
        row->setSub(QStringLiteral("#") + key.fingerprint);
        row->setChevron(false);
        row->setProperty(K_FINGERPRINT, key.fingerprint);
        switch (key.status) {
        case KeyStatus::Offered:
            row->setNote(TR("restore-key-ask"));
            break;
        case KeyStatus::Asking:
            row->setNote(TR("restore-device-trying"));
            break;
        case KeyStatus::Failed:
            row->setNote(key.error.isEmpty() ? TR("restore-key-failed") : key.error);
            break;
        }
        row->setClickable(key.status != KeyStatus::Asking);
        connect(row, &catalog::SelectRow::clicked, this, &RestoreAccount::onKeyRowClicked,
                qontrol::UNIQUE);
        m_key_rows->push(row);
    }
    refit();
}

auto RestoreAccount::keyAsked(ReqId req_id) -> KeyTry * {
    for (auto &key : m_keys_tried) {
        if (key.req != 0 && key.req == req_id) {
            return &key;
        }
    }
    return nullptr;
}

auto RestoreAccount::keyRead(backend::RequestId request_id) -> KeyTry * {
    for (auto &key : m_keys_tried) {
        if (key.read_req != 0 && key.read_req == request_id) {
            return &key;
        }
    }
    return nullptr;
}

auto RestoreAccount::scanKeyOnly() const -> bool {
    return !m_paths.isEmpty() && m_xpub_paths.isEmpty();
}

void RestoreAccount::stopDevices() {
    for (const auto &device : m_devices) {
        if (!device.signer.isNull()) {
            disconnect(device.signer, &ISigner::xpub, this, &RestoreAccount::onDeviceXpub);
            disconnect(device.signer, &ISigner::error, this, &RestoreAccount::onDeviceError);
        }
    }
    m_devices.clear();
}

void RestoreAccount::readWithXpub(const QString &xpub) {
    m_xpub_note->setVisible(false);
    if (xpub.trimmed().isEmpty() || m_target == nullptr) {
        m_xpub_input->setState(catalog::ValidationMark::State::None);
        m_xpub_req = 0;
        return;
    }
    m_xpub_req = nextRequestId();
    m_target->inspectBackup(m_xpub_req, m_backup, {xpub.trimmed()}, {});
}

// The backup opened with `xpubs` (none for a mnemonic), by `opener`: on to the review.
void RestoreAccount::opened(const QStringList &xpubs, const QString &opener) {
    m_xpubs = xpubs;
    m_opener = opener;
    m_xpub_req = 0;
    m_mnemonic_req = 0;
    leaveDecrypt();
}

// ===== Review view =====

void RestoreAccount::initReview() {
    m_review_opened = new catalog::Notice(catalog::Notice::Tone::Success);
    m_review_opened->setVisible(false);
    m_review_account = new catalog::Label(catalog::LabelRole::Body);
    m_review_account->setWordWrap(true);
    m_name_input = new catalog::Input;
    m_name_input->setWidth(Size::L);
    m_network_combo = new catalog::ComboBox;
    m_network_combo->setWidth(Size::L);
    // A backup leaves its network unsaid only when its keys are test keys.
    for (auto network :
         {backend::Network::Signet, backend::Network::Testnet, backend::Network::Regtest}) {
        m_network_combo->addItem(networkName(network), static_cast<int>(network));
    }
    m_review_fields = new catalog::FieldGroup;
    m_review_lists = new qontrol::Column;
    m_review_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_review_error->setVisible(false);
    m_restore = catalog::btnRestoreAccount();
    m_review_actions = new catalog::ModalActions;
    m_review_actions->setBack(catalog::ModalActions::Back::Back);
    m_review_actions->addAction(m_restore);
}

void RestoreAccount::connectReview() {
    connect(m_review_actions, &catalog::ModalActions::backClicked, this,
            &RestoreAccount::onBackClicked, qontrol::UNIQUE);
    connect(m_name_input, &QLineEdit::textEdited, this, &RestoreAccount::onNameEdited,
            qontrol::UNIQUE);
    connect(m_restore, &QPushButton::clicked, this, &RestoreAccount::onNextClicked,
            qontrol::UNIQUE);
    connect(m_target, &backend::IAccountBackend::restored, this, &RestoreAccount::onRestored,
            qontrol::UNIQUE);
}

auto RestoreAccount::viewReview() -> QWidget * {
    m_review_fields->addField(TR("restore-name-field"), m_name_input)
        ->addField(TR("restore-network-field"), m_network_combo);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_review_opened)
        ->push(m_review_account)
        ->push(m_review_fields)
        ->push(m_review_lists)
        ->push(m_review_error);
}

void RestoreAccount::showReview(const backend::BackupInfo &info) {
    m_view = View::Review;
    m_account = info.account;
    m_network = info.network;
    m_review_opened->setText(m_opener);
    m_review_opened->setVisible(!m_opener.isEmpty());
    QStringList summary{info.backend};
    if (info.network.has_value()) {
        summary.append(networkName(*info.network));
    }
    m_review_account->setText(summary.join(QStringLiteral(" · ")));
    m_name_input->setText(info.account);
    m_review_fields->setFieldVisible(m_network_combo, !info.network.has_value());
    catalog::clearRows(m_review_lists);
    m_review_lists->spacing(resolve(Spacing::M));
    m_review_lists->push(reviewList(TR("restore-includes"), info.includes));
    m_review_lists->push(reviewList(TR("restore-requirements"), info.requirements));
    for (const auto &warning : info.warnings) {
        auto *notice = new catalog::Notice(catalog::Notice::Tone::Warning);
        notice->setText(warning);
        m_review_lists->push(notice);
    }
    m_review_error->setVisible(false);
    onNameEdited();
    setHeader(nullptr);
    setBody(m_review_view);
    setFooter(m_review_actions);
    setWidth(ModalWidth::M);
}

void RestoreAccount::hideReview() {
    keep(m_review_view);
    keep(m_review_actions);
}

void RestoreAccount::restoreBackup() {
    if (m_target == nullptr || !m_restore->isEnabled()) {
        return;
    }
    auto network =
        m_network.value_or(static_cast<backend::Network>(m_network_combo->currentData().toInt()));
    m_account = m_name_input->text().trimmed();
    showBusy(TR("restore-restoring"));
    m_restore_req = nextRequestId();
    m_target->restore(m_restore_req, m_backup, m_xpubs, m_mnemonic, m_account, network);
}

auto RestoreAccount::reviewList(const QString &title, const QStringList &lines) -> QWidget * {
    auto *list = (new qontrol::Column)->spacing(resolve(Spacing::XS));
    for (const auto &line : lines) {
        auto *label = new catalog::Label(QStringLiteral("• ") + line);
        label->setWordWrap(true);
        list->push(label);
    }
    auto *section = new catalog::Section(title);
    section->setContent(list);
    return section;
}

// ===== Restored view =====

void RestoreAccount::initRestored() {
    m_restored_notice = new catalog::Notice(catalog::Notice::Tone::Success);
    m_done = catalog::btnDone();
    m_restored_actions = new catalog::ModalActions;
    m_restored_actions->addAction(m_done);
}

void RestoreAccount::connectRestored() {
    connect(m_done, &QPushButton::clicked, this, &RestoreAccount::onNextClicked, qontrol::UNIQUE);
}

auto RestoreAccount::viewRestored() -> QWidget * {
    return (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_restored_notice);
}

void RestoreAccount::showRestored() {
    m_view = View::Restored;
    m_restored_notice->setText(TR("restore-done").arg(m_account));
    setHeader(nullptr);
    setBody(m_restored_view);
    setFooter(m_restored_actions);
    setWidth(ModalWidth::M);
}

void RestoreAccount::hideRestored() {
    keep(m_restored_view);
    keep(m_restored_actions);
}

// ===== Shared =====

// The keys that opened the backup, and what was typed to get them.
void RestoreAccount::clearKeys() {
    m_xpubs.clear();
    m_mnemonic.fill('\0');
    m_mnemonic.clear();
    m_opener.clear();
    m_xpub_req = 0;
    m_mnemonic_req = 0;
    m_xpub_input->input()->clear();
    m_xpub_input->setState(catalog::ValidationMark::State::None);
    m_xpub_note->setVisible(false);
    m_mnemonic_input->input()->clear();
    m_mnemonic_input->setState(catalog::ValidationMark::State::None);
    m_mnemonic_note->setVisible(false);
    m_mnemonic_ack->setChecked(false);
}

// ===== Slots =====

void RestoreAccount::onBackClicked() {
    switch (m_view) {
    case View::Decrypt:
        leaveDecrypt();
        clearKeys();
        showFile();
        break;
    case View::Review:
        clearKeys();
        showFile();
        break;
    case View::File:
    case View::Busy:
    case View::Restored:
        break;
    }
}

void RestoreAccount::onNextClicked() {
    switch (m_view) {
    case View::File:
        readBackup();
        break;
    case View::Review:
        restoreBackup();
        break;
    case View::Restored:
        accept();
        break;
    case View::Busy:
    case View::Decrypt:
        break;
    }
}

void RestoreAccount::onChooseClicked() {
    auto path = QFileDialog::getOpenFileName(this, TR("restore-open-title"), QDir::homePath(),
                                             TR("restore-file-filter"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_file_error->setText(TR("restore-open-failed").arg(file.errorString()));
        m_file_error->setVisible(true);
        refit();
        return;
    }
    m_backup = file.readAll();
    m_backup_path = path;
    m_file_label->setText(QFileInfo(path).fileName());
    m_file_error->setVisible(false);
    showFile();
    refit();
}

void RestoreAccount::onBackupInspected(backend::RequestId request_id,
                                       const backend::BackupInfo &info) {
    if (request_id == 0) {
        return;
    }
    if (request_id == m_read_req) {
        // A plain backup: no key opened it.
        m_read_req = 0;
        showReview(info);
        return;
    }
    if (m_view != View::Decrypt) {
        return;
    }
    if (auto *device = deviceRead(request_id); device != nullptr) {
        // Copied: opened() drops the devices.
        auto xpubs = device->xpubs;
        auto opener = TR("restore-opened-device")
                          .arg(device->name, QStringLiteral("#") + device->fingerprint);
        opened(xpubs, opener);
        showReview(info);
    } else if (auto *key = keyRead(request_id); key != nullptr) {
        // Copied: opened() drops the keys asked.
        QStringList given{key->descriptor};
        auto opener = TR("restore-opened-device")
                          .arg(key->alias.isEmpty() ? key->fingerprint : key->alias,
                               QStringLiteral("#") + key->fingerprint);
        opened(given, opener);
        showReview(info);
    } else if (request_id == m_xpub_req) {
        opened({m_xpub_input->input()->text().trimmed()}, TR("restore-opened-xpub"));
        showReview(info);
    } else if (request_id == m_mnemonic_req) {
        m_mnemonic = m_mnemonic_input->input()->text().trimmed().toUtf8();
        opened({}, TR("restore-opened-mnemonic"));
        showReview(info);
    }
}

void RestoreAccount::onBackupLocked(backend::RequestId request_id,
                                    const QStringList &derivation_paths) {
    if (request_id == 0) {
        return;
    }
    if (request_id == m_read_req) {
        m_read_req = 0;
        showDecrypt(derivation_paths);
        return;
    }
    if (m_view != View::Decrypt) {
        return;
    }
    if (auto *device = deviceRead(request_id); device != nullptr) {
        device->read_req = 0;
        device->status = DeviceStatus::Failed;
        refreshDevices();
    } else if (auto *key = keyRead(request_id); key != nullptr) {
        key->read_req = 0;
        key->descriptor.clear();
        key->status = KeyStatus::Failed;
        key->error.clear();
        refreshKeyRows();
    } else if (request_id == m_xpub_req) {
        m_xpub_req = 0;
        m_xpub_input->setValid(false);
        m_xpub_note->setText(TR("restore-xpub-cannot-decrypt"));
        m_xpub_note->setVisible(true);
        refit();
    } else if (request_id == m_mnemonic_req) {
        m_mnemonic_req = 0;
        m_mnemonic_input->setValid(false);
        m_mnemonic_note->setText(TR("restore-mnemonic-cannot-decrypt"));
        m_mnemonic_note->setVisible(true);
        refit();
    }
}

void RestoreAccount::onRestored(backend::RequestId request_id) {
    if (request_id != m_restore_req) {
        return;
    }
    m_restore_req = 0;
    clearKeys();
    showRestored();
}

void RestoreAccount::onServiceError(backend::RequestId request_id, const QString &message) {
    if (request_id == 0) {
        return;
    }
    // A failed read goes back to the file, a failed restore back to the review.
    if (request_id == m_read_req) {
        m_read_req = 0;
        m_file_error->setText(message);
        m_file_error->setVisible(true);
        showFile();
    } else if (request_id == m_restore_req) {
        m_restore_req = 0;
        m_review_error->setText(TR("restore-failed").arg(message));
        m_review_error->setVisible(true);
        m_view = View::Review;
        setBody(m_review_view);
        setFooter(m_review_actions);
    } else if (auto *device = deviceRead(request_id); device != nullptr) {
        qWarning() << "restore: reading the backup with" << device->name << "failed:" << message;
        device->read_req = 0;
        device->status = DeviceStatus::Failed;
        refreshDevices();
    } else if (auto *key = keyRead(request_id); key != nullptr) {
        key->read_req = 0;
        key->descriptor.clear();
        key->status = KeyStatus::Failed;
        key->error = message;
        refreshKeyRows();
    } else if (request_id == m_xpub_req) {
        m_xpub_req = 0;
        m_xpub_input->setValid(false);
        m_xpub_note->setText(message);
        m_xpub_note->setVisible(true);
        refit();
    } else if (request_id == m_mnemonic_req) {
        m_mnemonic_req = 0;
        m_mnemonic_input->setValid(false);
        m_mnemonic_note->setText(message);
        m_mnemonic_note->setVisible(true);
        refit();
    }
}

void RestoreAccount::onInstanceChanged([[maybe_unused]] const QString &id) {
    if (m_view == View::Decrypt) {
        refreshDevices();
    }
}

void RestoreAccount::onDeviceXpub(ReqId req_id, const QString &xpub) {
    auto *device = deviceOf(sender(), req_id);
    if (device == nullptr) {
        return;
    }
    device->xpubs.append(xpub);
    ++device->path;
    fetchNext(*device);
    refreshDevices();
}

// A path the device refuses is skipped: another path may still open the backup.
void RestoreAccount::onDeviceError(std::optional<ReqId> req_id, const QString &message) {
    if (!req_id.has_value()) {
        return;
    }
    auto *device = deviceOf(sender(), *req_id);
    if (device == nullptr) {
        return;
    }
    qWarning() << "restore:" << device->name << "gave no xpub at"
               << m_xpub_paths.value(device->path) << ":" << message;
    ++device->path;
    fetchNext(*device);
    refreshDevices();
}

void RestoreAccount::onKeyRowClicked() {
    auto fingerprint = sender()->property(K_FINGERPRINT).toString();
    if (m_keys == nullptr || m_view != View::Decrypt) {
        return;
    }
    for (auto &key : m_keys_tried) {
        if (key.fingerprint != fingerprint || key.status == KeyStatus::Asking) {
            continue;
        }
        key.status = KeyStatus::Asking;
        key.error.clear();
        key.req = m_keys->newRequest();
        // Runs the key's own flow (its QR exchange, its password) and answers once it closes.
        m_keys->spDescriptor(key.req, key.network, key.fingerprint);
        break;
    }
    refreshKeyRows();
}

void RestoreAccount::onSpDescriptorReady(ReqId req_id, const QString &descriptor,
                                         [[maybe_unused]] const QString &fingerprint) {
    auto *key = keyAsked(req_id);
    if (key == nullptr || m_target == nullptr) {
        return;
    }
    key->req = 0;
    key->descriptor = descriptor;
    key->read_req = nextRequestId();
    m_target->inspectBackup(key->read_req, m_backup, {descriptor}, {});
}

void RestoreAccount::onKeysError(std::optional<ReqId> req_id, const QString &message) {
    if (!req_id.has_value()) {
        return;
    }
    auto *key = keyAsked(*req_id);
    if (key == nullptr) {
        return;
    }
    key->req = 0;
    key->status = KeyStatus::Failed;
    key->error = message;
    refreshKeyRows();
}

void RestoreAccount::onKeysCancelled(ReqId req_id) {
    auto *key = keyAsked(req_id);
    if (key == nullptr) {
        return;
    }
    key->req = 0;
    key->status = KeyStatus::Offered;
    refreshKeyRows();
}

void RestoreAccount::onXpubEdited() {
    readWithXpub(m_xpub_input->input()->text());
}

void RestoreAccount::onPasteXpubClicked() {
    m_xpub_input->input()->setText(QApplication::clipboard()->text().trimmed());
    onXpubEdited();
}

void RestoreAccount::onImportXpubClicked() {
    auto path =
        QFileDialog::getOpenFileName(this, TR("restore-import-xpub-title"), QDir::homePath());
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_xpub_note->setText(TR("restore-open-failed").arg(file.errorString()));
        m_xpub_note->setVisible(true);
        refit();
        return;
    }
    m_xpub_input->input()->setText(QString::fromUtf8(file.readAll()).trimmed());
    onXpubEdited();
}

void RestoreAccount::onMnemonicAckToggled(bool acknowledged) {
    m_mnemonic_input->setEnabled(acknowledged);
    if (!acknowledged) {
        m_mnemonic_input->input()->clear();
        onMnemonicEdited();
    }
}

void RestoreAccount::onMnemonicEdited() {
    m_mnemonic_note->setVisible(false);
    auto mnemonic = m_mnemonic_input->input()->text().trimmed().toUtf8();
    if (mnemonic.isEmpty() || m_target == nullptr) {
        m_mnemonic_input->setState(catalog::ValidationMark::State::None);
        m_mnemonic_req = 0;
        return;
    }
    m_mnemonic_req = nextRequestId();
    m_target->inspectBackup(m_mnemonic_req, m_backup, {}, mnemonic);
    mnemonic.fill('\0');
}

void RestoreAccount::onNameEdited() {
    bool named = !m_name_input->text().trimmed().isEmpty();
    m_restore->setEnabled(named);
    m_review_actions->setReason(named ? QString() : TR("restore-enter-name"));
}

} // namespace account::view
