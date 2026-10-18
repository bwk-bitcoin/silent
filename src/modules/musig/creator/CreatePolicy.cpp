#include "modules/musig/creator/CreatePolicy.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/InfoTip.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/display/WizardSteps.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/Toggle.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "modules/keys/KeysView.h"
#include "modules/musig/MusigModule.h"
#include "modules/musig/creator/PathEditor.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QRegularExpression>
#include <QThread>
#include <Qontrol>
#include <array>
#include <common.h>
#include <string>

namespace musig::modal {

namespace {

constexpr std::array<Network, 4> NETWORKS = {Network::Regtest, Network::Signet, Network::Testnet,
                                             Network::Bitcoin};

// exec() result of a window closed to run a keys-module flow in its place.
constexpr int FLOW_RESULT = QDialog::Accepted + 1;

constexpr std::uint64_t DUST_LIMIT = 546;

const char *const K_INDEX = "pathIndex";
const char *const K_FINGERPRINT = "fingerprint";

const QString K_SSL_SCHEME = QStringLiteral("ssl://");
const QString K_TCP_SCHEME = QStringLiteral("tcp://");

auto stepNames() -> QStringList {
    return {TR("musig-step-paths"),    TR("musig-step-xpubs"),  TR("musig-step-review"),
            TR("musig-step-register"), TR("musig-step-verify"), TR("musig-step-electrum")};
}

auto accountExists(const QString &name) -> bool {
    auto configs = ::list_configs();
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const auto &existing : configs) {
        if (QString::fromStdString(std::string(existing)) == name) {
            return true;
        }
    }
    return false;
}

// Why compile_policy() or preview_policy() refused the policy.
auto compileError(const PolicyCompileResult &result) -> QString {
    if (result.too_many_leaves) {
        return TR("musig-create-too-many-leaves").arg(result.leaf_count).arg(result.max_leaves);
    }
    if (result.error_path >= 0) {
        return TR("musig-create-path-error")
            .arg(result.error_path + 1)
            .arg(QString::fromStdString(std::string(result.error)));
    }
    return QString::fromStdString(std::string(result.error));
}

auto lead(const QString &text) -> catalog::Label * {
    auto *label = new catalog::Label(text, catalog::LabelRole::Body);
    label->setWordWrap(true);
    return label;
}

// A key of the paths as a row: its route icon, its name over its fingerprint, and
// `status` on the right. Clickable only when `clickable`.
auto keyRow(const KeyEntry &entry, QWidget *status, bool clickable) -> catalog::SelectRow * {
    auto *row = new catalog::SelectRow;
    row->setIcon(catalog::routeIcon(entry.route));
    row->setTitle(entry.alias.isEmpty() ? entry.fingerprint : entry.alias);
    row->setSub(entry.fingerprint);
    row->setStatus(status);
    row->setChevron(false);
    row->setClickable(clickable);
    row->setProperty(K_FINGERPRINT, entry.fingerprint);
    return row;
}

// The paths in words, as Review shows them: one card per path, its signers, its
// timelock, and its keys with the mandatory ones marked.
auto policyCard(const SpendPath &path, int index, const QHash<QString, QString> &names,
                std::optional<std::uint32_t> tip) -> QWidget * {
    auto *title = new catalog::Label(
        TR("musig-review-path").arg(pathTitle(path, index), signerWords(path, names)),
        catalog::LabelRole::Subheading);
    title->setWordWrap(true);
    auto *lock = new catalog::Label(lockLine(path, tip), catalog::LabelRole::Caption);
    lock->setTone(catalog::LabelTone::Secondary);
    auto *content = (new qontrol::Column)->spacing(resolve(Spacing::XXS))->push(title)->push(lock);
    for (const auto &fingerprint : path.keys) {
        auto *name = new catalog::Label(names.value(fingerprint, fingerprint));
        auto *origin = new catalog::Label(fingerprint, catalog::LabelRole::Mono);
        origin->setTone(catalog::LabelTone::Muted);
        auto *row = (new qontrol::Row)->spacing(resolve(Spacing::S))->push(name)->push(origin);
        if (path.mandatory.contains(fingerprint)) {
            auto *mandatory =
                new catalog::Label(TR("musig-path-mandatory"), catalog::LabelRole::Caption);
            row->push(mandatory);
        }
        content->push(row->pushSpacer());
    }
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setContent(content);
    return card;
}

} // namespace

CreatePolicy::CreatePolicy() {
    init();
    doConnect();
    view();
    applyNetworkDefaults();
    fetchTip();
    showPaths();
}

auto CreatePolicy::exec() -> int {
    int result = catalog::Modal::exec();
    while (result == FLOW_RESULT) {
        runFlow();
        result = catalog::Modal::exec();
    }
    return result;
}

void CreatePolicy::init() {
    m_steps = new catalog::WizardSteps(stepNames());
    m_status_label = new catalog::Label(catalog::LabelRole::Caption);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    initPaths();
    initEditPath();
    initXpubs();
    initReview();
    initRegister();
    initVerify();
    initElectrum();
}

void CreatePolicy::doConnect() {
    connect(this, &CreatePolicy::tipReady, this, &CreatePolicy::onTipReady, qontrol::UNIQUE);
    for (auto *actions : {m_edit_actions, m_xpubs_actions, m_review_actions, m_register_actions,
                          m_verify_actions, m_electrum_actions}) {
        connect(actions, &catalog::ModalActions::backClicked, this, &CreatePolicy::onBackClicked,
                qontrol::UNIQUE);
    }
    for (auto *next : {m_paths_next_btn, m_save_path_btn, m_xpubs_next_btn, m_review_next_btn,
                       m_register_next_btn, m_verify_next_btn, m_install_btn}) {
        connect(next, &QPushButton::clicked, this, &CreatePolicy::onNextClicked, qontrol::UNIQUE);
    }
    auto *bus = Host::get()->keys();
    if (bus != nullptr) {
        connect(bus, &IKeys::xpubReady, this, &CreatePolicy::onXpubReady, qontrol::UNIQUE);
        connect(bus, &IKeys::descriptorRegistered, this, &CreatePolicy::onDescriptorRegistered,
                qontrol::UNIQUE);
        connect(bus, &IKeys::verified, this, &CreatePolicy::onAddressVerified, qontrol::UNIQUE);
        connect(bus, &IKeys::error, this, &CreatePolicy::onKeysError, qontrol::UNIQUE);
        connect(bus, &IKeys::cancelled, this, &CreatePolicy::onKeysCancelled, qontrol::UNIQUE);
    }
    connectPaths();
    connectEditPath();
    connectReview();
    connectRegister();
    connectElectrum();
}

void CreatePolicy::view() {
    setWidth(ModalWidth::Wizard);
    setWizardHeight(true);
    m_paths_page = viewPaths();
    m_edit_page = viewEditPath();
    m_xpubs_page = viewXpubs();
    m_review_page = viewReview();
    m_register_page = viewRegister();
    m_verify_page = viewVerify();
    m_electrum_page = viewElectrum();
    keep((new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_steps)->push(m_status_label));
    hideEditPath();
    hideXpubs();
    hideReview();
    hideRegister();
    hideVerify();
    hideElectrum();
}

// ===== Paths view =====

void CreatePolicy::initPaths() {
    m_network_combo = new catalog::ComboBox;
    m_network_combo->setWidth(Size::M);
    for (auto network : NETWORKS) {
        m_network_combo->addItem(keys::networkLabel(network), static_cast<int>(network));
    }
    m_path_rows = new qontrol::Column;
    m_path_rows->spacing(resolve(Spacing::XS));
    m_paths_next_btn = catalog::btnContinue();
    m_paths_actions = new catalog::ModalActions;
    m_paths_actions->addAction(m_paths_next_btn);
    m_paths.append(defaultPath());
}

void CreatePolicy::connectPaths() {
    connect(m_network_combo, &QComboBox::currentIndexChanged, this, &CreatePolicy::onNetworkChanged,
            qontrol::UNIQUE);
}

auto CreatePolicy::viewPaths() -> QWidget * {
    auto *network = (new catalog::FieldGroup)->addField(TR("settings-network"), m_network_combo);
    m_paths_footer = m_paths_actions;
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push((new qontrol::Row)->pushSpacer()->push(network))
        ->push(m_path_rows)
        ->pushSpacer();
}

void CreatePolicy::showPaths() {
    m_view = View::Paths;
    showStep(0);
    refreshPathRows();
    setWideBody(m_paths_page);
    setFooter(m_paths_footer);
    m_paths_actions->setReason(pathsReason());
    m_paths_next_btn->setEnabled(pathsReason().isEmpty());
}

void CreatePolicy::refreshPathRows() {
    // clear() also resets the layout spacing.
    m_path_rows->clear();
    m_path_rows->spacing(resolve(Spacing::XS));
    auto names = keyNames();
    for (int i = 0; i < m_paths.size(); ++i) {
        const auto &path = m_paths.at(i);
        auto *row = new catalog::SelectRow;
        row->setTitle(pathTitle(path, i));
        row->setSub(lockLine(path, m_tip));
        row->setMeta(signerWords(path, names));
        row->setProperty(K_INDEX, i);
        connect(row, &catalog::SelectRow::clicked, this, &CreatePolicy::onPathRowClicked,
                qontrol::UNIQUE);
        m_path_rows->push(row);
        row->show();
    }
    auto *add = new catalog::SelectRow;
    add->setTone(catalog::SelectRow::Tone::Add);
    add->setIcon(icon::plus);
    add->setTitle(TR("musig-create-add-path"));
    connect(add, &catalog::SelectRow::clicked, this, &CreatePolicy::onAddPathClicked,
            qontrol::UNIQUE);
    m_path_rows->push(add);
    add->show();
}

auto CreatePolicy::pathsReason() -> QString {
    if (networkKeys().isEmpty()) {
        return TR("musig-create-reason-keys");
    }
    for (int i = 0; i < m_paths.size(); ++i) {
        const auto &path = m_paths.at(i);
        if (path.keys.isEmpty()) {
            return TR("musig-create-pick-keys");
        }
        auto reason = lockReason(path, m_tip);
        if (!reason.isEmpty()) {
            return TR("musig-create-path-error").arg(i + 1).arg(reason);
        }
    }
    auto result = ::preview_policy(spec());
    return result.is_ok ? QString() : compileError(result);
}

// ===== EditPath view =====

void CreatePolicy::initEditPath() {
    m_editor = new PathEditor(networkKeys());
    m_remove_path_btn = catalog::btnRemoveSpendPath();
    m_save_path_btn = catalog::btnSaveSpendPath();
    m_edit_actions = new catalog::ModalActions;
    m_edit_actions->setBack(catalog::ModalActions::Back::Back);
    m_edit_actions->addAction(m_remove_path_btn);
    m_edit_actions->addAction(m_save_path_btn);
}

void CreatePolicy::connectEditPath() {
    connect(m_editor, &PathEditor::edited, this, &CreatePolicy::onPathEdited, qontrol::UNIQUE);
    connect(m_remove_path_btn, &QPushButton::clicked, this, &CreatePolicy::onRemovePathClicked,
            qontrol::UNIQUE);
}

auto CreatePolicy::viewEditPath() -> QWidget * {
    m_edit_footer = m_edit_actions;
    return (new qontrol::Column)->push(m_editor)->pushSpacer();
}

void CreatePolicy::showEditPath(int index) {
    m_view = View::EditPath;
    m_editing = index;
    bool isNew = index >= m_paths.size();
    SpendPath path;
    if (isNew) {
        // A new path: a relative timelock in days, as a recovery path mostly is.
        path.lock = LockKind::Relative;
        path.unit = LockUnit::Days;
    } else {
        path = m_paths.at(index);
    }
    m_editor->setTip(m_tip);
    m_editor->setPath(path);
    setWindowTitle(pathTitle(path, index));
    setHeader(nullptr);
    setWideBody(m_edit_page);
    setFooter(m_edit_footer);
    m_remove_path_btn->setVisible(!isNew && m_paths.size() > 1);
    refreshEditActions();
}

void CreatePolicy::hideEditPath() {
    keep(m_edit_page);
    keep(m_edit_footer);
}

void CreatePolicy::refreshEditActions() {
    auto path = m_editor->path();
    QString reason;
    if (path.keys.isEmpty()) {
        reason = TR("musig-path-reason-keys");
    } else {
        reason = lockReason(path, m_tip);
    }
    m_edit_actions->setReason(reason);
    m_save_path_btn->setEnabled(reason.isEmpty());
    setWindowTitle(pathTitle(path, m_editing));
}

// ===== Xpubs view =====

void CreatePolicy::initXpubs() {
    m_xpubs_count = new catalog::Label(catalog::LabelRole::Caption);
    m_xpubs_count->setTone(catalog::LabelTone::Secondary);
    m_xpub_rows = new qontrol::Column;
    m_xpub_rows->spacing(resolve(Spacing::XS));
    m_xpubs_next_btn = catalog::btnContinue();
    m_xpubs_actions = new catalog::ModalActions;
    m_xpubs_actions->setBack(catalog::ModalActions::Back::Back);
    m_xpubs_actions->addAction(m_xpubs_next_btn);
}

auto CreatePolicy::viewXpubs() -> QWidget * {
    m_xpubs_footer = m_xpubs_actions;
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::S))
        ->push(lead(TR("musig-xpubs-lead")))
        ->push(m_xpubs_count)
        ->push(m_xpub_rows)
        ->pushSpacer();
}

void CreatePolicy::showXpubs() {
    m_view = View::Xpubs;
    showStep(1);
    setWideBody(m_xpubs_page);
    setFooter(m_xpubs_footer);
    refreshXpubRows();
}

void CreatePolicy::hideXpubs() {
    keep(m_xpubs_page);
    keep(m_xpubs_footer);
}

void CreatePolicy::refreshXpubRows() {
    auto missing = missingXpubs();
    m_xpubs_count->setText(TR("musig-create-fetch-status")
                               .arg(m_fingerprints.size() - missing.size())
                               .arg(m_fingerprints.size()));
    // clear() also resets the layout spacing.
    m_xpub_rows->clear();
    m_xpub_rows->spacing(resolve(Spacing::XS));
    for (const auto &fingerprint : m_fingerprints) {
        auto entry = keyEntry(fingerprint).value_or(KeyEntry{.fingerprint = fingerprint});
        bool fetched = !missing.contains(fingerprint);
        bool fetching = asking(Ask::Xpub, fingerprint);
        auto failure = m_xpub_failed.value(fingerprint);
        catalog::Badge *status = nullptr;
        if (fetched) {
            status = catalog::pillXpubFetched();
        } else if (fetching) {
            status = catalog::pillMusigOnDevice();
        } else if (!failure.isEmpty()) {
            status = catalog::pillRegFailed();
        } else {
            status = catalog::pillXpubMissing();
        }
        auto *row = keyRow(entry, status, !fetched && !busy(fingerprint));
        // A failed device says why on its own row; a click tries it again.
        if (!fetched && !fetching && !failure.isEmpty()) {
            markFailed(row, failure);
        }
        connect(row, &catalog::SelectRow::clicked, this, &CreatePolicy::onXpubRowClicked,
                qontrol::UNIQUE);
        m_xpub_rows->push(row);
        row->show();
    }
    QString reason;
    if (missing.size() == 1) {
        reason = TR("musig-xpubs-reason-one");
    } else if (missing.size() > 1) {
        reason = TR("musig-xpubs-reason").arg(missing.size());
    }
    m_xpubs_actions->setReason(reason);
    m_xpubs_next_btn->setEnabled(missing.isEmpty());
}

// ===== Review view =====

void CreatePolicy::initReview() {
    m_name_input = new catalog::Input;
    m_name_input->setWidth(Size::M);
    m_name_input->setPlaceholderText(TR("create-account-name-placeholder"));
    m_review_cards = new qontrol::Column;
    m_review_cards->spacing(resolve(Spacing::S));
    m_backed_up_label =
        new catalog::Label(TR("musig-review-backed-up"), catalog::LabelRole::Caption);
    m_backed_up_label->setTone(catalog::LabelTone::Success);
    m_backup_primary_btn = catalog::btnBackupDescriptorPrimary();
    m_backup_btn = catalog::btnBackupDescriptor();
    m_privacy_check = new catalog::Checkbox(TR("musig-review-privacy-mode"));
    // On by default: the backup's clear part names no derivation path unless asked to.
    m_privacy_check->setChecked(true);
    m_backup_note = new catalog::Label(catalog::LabelRole::Caption);
    m_backup_note->setTone(catalog::LabelTone::Warning);
    m_backup_note->setWordWrap(true);
    m_backup_note->setVisible(false);
    m_review_next_btn = catalog::btnContinue();
    m_review_actions = new catalog::ModalActions;
    m_review_actions->setBack(catalog::ModalActions::Back::Back);
    m_review_actions->addAction(m_review_next_btn);
}

void CreatePolicy::connectReview() {
    connect(m_name_input, &QLineEdit::textChanged, this, &CreatePolicy::onNameEdited,
            qontrol::UNIQUE);
    for (auto *backup : {m_backup_primary_btn, m_backup_btn}) {
        connect(backup, &QPushButton::clicked, this, &CreatePolicy::onBackupClicked,
                qontrol::UNIQUE);
    }
}

auto CreatePolicy::viewReview() -> QWidget * {
    auto *name = (new catalog::FieldGroup)->addField(TR("create-account-name"), m_name_input);
    // The privacy mode on the button's own row: it sets what the button saves.
    auto *backup = (new qontrol::Row)
                       ->spacing(resolve(Spacing::S))
                       ->push((new qontrol::Row)
                                  ->spacing(resolve(Spacing::XS))
                                  ->push(m_privacy_check)
                                  ->push(new catalog::InfoTip(TR("musig-review-privacy-tip"))))
                       ->pushSpacer()
                       ->push(m_backed_up_label)
                       ->push(m_backup_primary_btn)
                       ->push(m_backup_btn);
    m_review_footer = m_review_actions;
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(name)
        ->push(m_review_cards)
        ->push(
            (new qontrol::Column)->spacing(resolve(Spacing::S))->push(backup)->push(m_backup_note))
        ->pushSpacer();
}

void CreatePolicy::showReview() {
    m_view = View::Review;
    showStep(2);
    // clear() also resets the layout spacing.
    m_review_cards->clear();
    m_review_cards->spacing(resolve(Spacing::S));
    auto names = keyNames();
    for (int i = 0; i < m_paths.size(); ++i) {
        auto *card = policyCard(m_paths.at(i), i, names, m_tip);
        m_review_cards->push(card);
        card->show();
    }
    setWideBody(m_review_page);
    setFooter(m_review_footer);
    refreshReviewActions();
}

void CreatePolicy::hideReview() {
    keep(m_review_page);
    keep(m_review_footer);
}

void CreatePolicy::refreshReviewActions() {
    QString name = m_name_input->text();
    QString trimmed = name.trimmed();
    QString nameReason;
    if (trimmed.isEmpty()) {
        nameReason = TR("create-account-reason-name");
    } else if (name.contains(QRegularExpression(QStringLiteral("\\s")))) {
        nameReason = TR("create-account-reason-spaces");
    } else if (accountExists(trimmed)) {
        nameReason = TR("create-account-reason-exists").arg(trimmed);
    }
    m_backup_primary_btn->setVisible(!m_backed_up);
    m_backup_btn->setVisible(m_backed_up);
    m_backed_up_label->setVisible(m_backed_up);
    m_backup_primary_btn->setEnabled(nameReason.isEmpty());
    m_backup_btn->setEnabled(nameReason.isEmpty());
    auto reason = nameReason;
    if (reason.isEmpty() && !m_backed_up) {
        reason = TR("musig-review-reason-backup");
    }
    m_review_actions->setReason(reason);
    m_review_next_btn->setEnabled(reason.isEmpty());
}

// ===== Register view =====

void CreatePolicy::initRegister() {
    m_register_rows = new qontrol::Column;
    m_register_rows->spacing(resolve(Spacing::XS));
    m_skip_check = new catalog::Checkbox(TR("musig-register-skip"));
    m_register_next_btn = catalog::btnContinue();
    m_register_actions = new catalog::ModalActions;
    m_register_actions->setBack(catalog::ModalActions::Back::Back);
    m_register_actions->addAction(m_register_next_btn);
}

void CreatePolicy::connectRegister() {
    connect(m_skip_check, &QCheckBox::toggled, this, &CreatePolicy::onSkipToggled, qontrol::UNIQUE);
}

auto CreatePolicy::viewRegister() -> QWidget * {
    m_register_footer = (new qontrol::Column)
                            ->spacing(resolve(Spacing::M))
                            ->push(m_skip_check)
                            ->push(m_register_actions);
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::S))
        ->push(lead(TR("musig-register-lead")))
        ->push(m_register_rows)
        ->pushSpacer();
}

void CreatePolicy::showRegister() {
    m_view = View::Register;
    showStep(3);
    setWideBody(m_register_page);
    setFooter(m_register_footer);
    refreshRegisterRows();
    registerNextQuiet();
}

void CreatePolicy::hideRegister() {
    keep(m_register_page);
    keep(m_register_footer);
}

void CreatePolicy::refreshRegisterRows() {
    // clear() also resets the layout spacing.
    m_register_rows->clear();
    m_register_rows->spacing(resolve(Spacing::XS));
    int left = 0;
    for (const auto &fingerprint : m_fingerprints) {
        auto entry = keyEntry(fingerprint).value_or(KeyEntry{.fingerprint = fingerprint});
        bool needs = needsRegister(fingerprint);
        bool done = m_registered.contains(fingerprint);
        if (needs && !done) {
            ++left;
        }
        auto failure = m_register_failed.value(fingerprint);
        catalog::Badge *status = nullptr;
        if (!needs) {
            status = catalog::pillRegNotNeeded();
        } else if (done) {
            status = catalog::pillRegRegistered();
        } else if (asking(Ask::Register, fingerprint)) {
            status = catalog::pillRegRegistering();
        } else if (!failure.isEmpty()) {
            status = catalog::pillRegFailed();
        } else {
            status = catalog::pillRegPending();
        }
        auto *row = keyRow(entry, status, needs && !done && !busy(fingerprint));
        row->setEnabled(needs);
        // A failed device says why on its own row; a click tries it again.
        if (needs && !done && !asking(Ask::Register, fingerprint) && !failure.isEmpty()) {
            markFailed(row, failure);
        }
        connect(row, &catalog::SelectRow::clicked, this, &CreatePolicy::onRegisterRowClicked,
                qontrol::UNIQUE);
        m_register_rows->push(row);
        row->show();
    }
    m_skip_check->setVisible(left > 0);
    bool registering = askingAny(Ask::Register);
    QString reason;
    if (registering) {
        reason = TR("musig-register-reason-busy");
    } else if (left > 0 && !m_skip_check->isChecked()) {
        reason =
            left == 1 ? TR("musig-register-reason-one") : TR("musig-register-reason").arg(left);
    }
    m_register_actions->setReason(reason);
    m_register_actions->setBack(registering ? catalog::ModalActions::Back::None
                                            : catalog::ModalActions::Back::Back);
    m_register_next_btn->setEnabled(reason.isEmpty());
}

void CreatePolicy::registerNextQuiet() {
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return;
    }
    // m_registered is read again each time round: an answer may come during the call.
    for (const auto &fingerprint : m_fingerprints) {
        if (!needsRegister(fingerprint) && !m_registered.contains(fingerprint) &&
            !busy(fingerprint)) {
            auto req = beginAsk(Ask::Register, fingerprint);
            // No window: a key with no device records the descriptor at once.
            keys->registerDescriptor(req, network(), fingerprint, m_descriptor);
        }
    }
}

// ===== Verify view =====

void CreatePolicy::initVerify() {
    m_verify_lead = lead(TR("musig-create-verify-lead"));
    m_address_label = new catalog::ChunkedAddress;
    m_address_label->setLineMode(catalog::ChunkedAddress::LineMode::Single);
    m_verify_rows = new qontrol::Column;
    m_verify_rows->spacing(resolve(Spacing::XS));
    m_skip_notice = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_skip_notice->setTitle(TR("musig-create-skip-verify-title"));
    m_skip_notice->setText(TR("musig-create-skip-verify"));
    m_verify_next_btn = catalog::btnContinue();
    m_verify_actions = new catalog::ModalActions;
    m_verify_actions->setBack(catalog::ModalActions::Back::Back);
    m_verify_actions->addAction(m_verify_next_btn);
}

auto CreatePolicy::viewVerify() -> QWidget * {
    auto *addressCard = new catalog::Card(catalog::Card::Role::Inset);
    addressCard->setContent(m_address_label);
    m_verify_footer = m_verify_actions;
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_verify_lead)
        ->push((new qontrol::Row)->pushSpacer()->push(addressCard)->pushSpacer())
        ->push(m_verify_rows)
        ->push(m_skip_notice)
        ->pushSpacer();
}

void CreatePolicy::showVerify() {
    m_view = View::Verify;
    showStep(4);
    m_address_label->setAddress(QString::fromStdString(std::string(m_address.address)));
    setWideBody(m_verify_page);
    setFooter(m_verify_footer);
    refreshVerifyRows();
}

void CreatePolicy::hideVerify() {
    keep(m_verify_page);
    keep(m_verify_footer);
}

void CreatePolicy::refreshVerifyRows() {
    // clear() also resets the layout spacing.
    m_verify_rows->clear();
    m_verify_rows->spacing(resolve(Spacing::XS));
    bool checkable = false;
    for (const auto &fingerprint : m_fingerprints) {
        auto entry = keyEntry(fingerprint).value_or(KeyEntry{.fingerprint = fingerprint});
        bool screen = entry.can_show_address;
        checkable = checkable || screen;
        bool checking = asking(Ask::Verify, fingerprint);
        auto failure = m_verify_failed.value(fingerprint);
        catalog::Badge *status = nullptr;
        if (!screen) {
            status = catalog::pillAddrNoScreen();
        } else if (m_verified.contains(fingerprint)) {
            status = catalog::pillAddrVerified();
        } else if (checking) {
            status = catalog::pillMusigOnDevice();
        } else if (m_mismatched.contains(fingerprint)) {
            status = catalog::pillAddrMismatch();
        } else if (!failure.isEmpty()) {
            status = catalog::pillRegFailed();
        } else {
            status = catalog::pillAddrUnchecked();
        }
        auto *row = keyRow(entry, status, screen && !busy(fingerprint));
        row->setEnabled(screen);
        // A device that did not confirm says why on its own row; a click asks it again.
        if (screen && !checking && !m_verified.contains(fingerprint) && !failure.isEmpty()) {
            markFailed(row, failure);
        }
        connect(row, &catalog::SelectRow::clicked, this, &CreatePolicy::onVerifyRowClicked,
                qontrol::UNIQUE);
        m_verify_rows->push(row);
        row->show();
    }
    m_verify_lead->setText(checkable ? TR("musig-create-verify-lead")
                                     : TR("musig-create-verify-lead-unchecked"));
    m_skip_notice->setVisible(!checkable);
}

// ===== Electrum view =====

void CreatePolicy::initElectrum() {
    m_ssl_toggle = new catalog::Toggle;
    m_verify_toggle = new catalog::Toggle;
    m_verify_toggle->setChecked(true);
    m_install_btn = catalog::btnCreate();
    m_electrum_actions = new catalog::ModalActions;
    m_electrum_actions->setBack(catalog::ModalActions::Back::Back);
    m_electrum_actions->addAction(m_install_btn);
}

void CreatePolicy::connectElectrum() {
    connect(m_electrum.testButton(), &QPushButton::clicked, this, &CreatePolicy::onTestElectrum,
            qontrol::UNIQUE);
    connect(m_electrum.input(), &QLineEdit::textEdited, this, &CreatePolicy::onElectrumEdited,
            qontrol::UNIQUE);
    connect(this, &CreatePolicy::electrumTestReady, this, &CreatePolicy::onElectrumTestReady,
            qontrol::UNIQUE);
    connect(m_ssl_toggle, &QCheckBox::toggled, this, &CreatePolicy::onSslToggled, qontrol::UNIQUE);
    connect(m_verify_toggle, &QCheckBox::toggled, this, &CreatePolicy::onVerifyCertificateToggled,
            qontrol::UNIQUE);
}

auto CreatePolicy::viewElectrum() -> QWidget * {
    auto *notice = new catalog::Notice(catalog::Notice::Tone::Warning);
    notice->setTitle(TR("musig-electrum-own-title"));
    notice->setText(TR("musig-electrum-own"));
    auto *fields = (new catalog::FieldGroup)
                       ->addField(TR("create-account-electrum"), m_electrum.control())
                       ->addNote(m_electrum.note())
                       ->addField(TR("musig-electrum-ssl"), m_ssl_toggle)
                       ->addTippedField(TR("musig-electrum-verify-certificate"),
                                        new catalog::InfoTip(TR("musig-electrum-verify-tip")),
                                        m_verify_toggle);
    m_electrum_footer = m_electrum_actions;
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(lead(TR("musig-electrum-lead")))
        ->push(notice)
        ->push(fields)
        ->pushSpacer();
}

void CreatePolicy::showElectrum() {
    m_view = View::Electrum;
    showStep(5);
    setWideBody(m_electrum_page);
    setFooter(m_electrum_footer);
    refreshElectrumActions();
}

void CreatePolicy::hideElectrum() {
    keep(m_electrum_page);
    keep(m_electrum_footer);
}

void CreatePolicy::refreshElectrumActions() {
    m_verify_toggle->setEnabled(m_ssl_toggle->isChecked());
    QString reason;
    if (m_electrum.state() != catalog::EndpointRow::State::Ok) {
        reason = TR("musig-create-reason-electrum");
    }
    m_electrum_actions->setReason(reason);
    m_install_btn->setEnabled(reason.isEmpty());
}

void CreatePolicy::applyNetworkDefaults() {
    // The server the last account of this network was created with comes first.
    auto electrum = QString::fromStdString(
        std::string(::app_last_endpoint(BackendServer::Electrum, network())));
    if (electrum.isEmpty() && network() == Network::Regtest) {
        electrum = REGTEST_ELECTRUM_URL;
    }
    m_electrum.input()->setText(electrum);
    m_electrum.setState(catalog::EndpointRow::State::Untested);
    // Test the default rather than assume it: only a passing test resolves the scheme
    // that gets saved, and sets SSL.
    onTestElectrum();
}

auto CreatePolicy::electrumAddress() const -> QString {
    auto address = m_electrum.input()->text().trimmed();
    bool stated = address.contains(QStringLiteral("://"));
    // A bare address is the default's: the test tries both schemes and SSL follows.
    if (!stated) {
        return address;
    }
    auto bare = address.section(QStringLiteral("://"), 1);
    return (m_ssl_toggle->isChecked() ? K_SSL_SCHEME : K_TCP_SCHEME) + bare;
}

void CreatePolicy::install() {
    auto name = m_name_input->text().trimmed();
    if (accountExists(name)) {
        showStatus(TR("create-account-reason-exists").arg(name), catalog::LabelTone::Error);
        return;
    }
    auto electrumUrl = m_electrum.input()->text().trimmed();
    auto config = new_config(rust::String(name.toStdString()), network(),
                             rust::String(m_descriptor.toStdString()), rust::String(),
                             rust::String(electrumUrl.toStdString()), DUST_LIMIT,
                             rust::String(MUSIG_PLUGIN_ID));
    // Only an SSL server presents a certificate to check.
    config->set_verify_certificate(!m_ssl_toggle->isChecked() || m_verify_toggle->isChecked());
    config->to_file();
    // Prefilled for the next account of this network.
    ::app_set_last_endpoint(BackendServer::Electrum, network(),
                            rust::String(electrumUrl.toStdString()));
    Host::get()->refreshLauncher();
    accept();
}

// ===== Shared =====

void CreatePolicy::showStep(int index) {
    m_steps->setCurrent(index);
    setWindowTitle(TR("musig-create-title"));
    showStatus({}, catalog::LabelTone::Muted);
    setHeader(m_steps->parentWidget());
}

auto CreatePolicy::network() const -> Network {
    return static_cast<Network>(m_network_combo->currentData().toInt());
}

auto CreatePolicy::networkKeys() const -> QList<KeyEntry> {
    auto *bus = Host::get()->keys();
    return bus != nullptr ? bus->keys(network()) : QList<KeyEntry>{};
}

auto CreatePolicy::defaultPath() const -> SpendPath {
    SpendPath first;
    for (const auto &entry : networkKeys()) {
        if (entry.can_musig) {
            first.keys.append(entry.fingerprint);
        }
    }
    first.threshold = first.keys.size() > 1 ? 2 : 1;
    first.lock = LockKind::None;
    return first;
}

auto CreatePolicy::keyNames() const -> QHash<QString, QString> {
    QHash<QString, QString> names;
    for (const auto &entry : networkKeys()) {
        names.insert(entry.fingerprint, entry.alias.isEmpty() ? entry.fingerprint : entry.alias);
    }
    return names;
}

auto CreatePolicy::keyEntry(const QString &fingerprint) const -> std::optional<KeyEntry> {
    for (const auto &entry : networkKeys()) {
        if (entry.fingerprint == fingerprint) {
            return entry;
        }
    }
    return std::nullopt;
}

void CreatePolicy::fetchTip() {
    m_tip.reset();
    auto address = QString::fromStdString(
        std::string(::app_last_endpoint(BackendServer::Electrum, network())));
    if (address.isEmpty() && network() == Network::Regtest) {
        address = REGTEST_ELECTRUM_URL;
    }
    if (address.isEmpty()) {
        return;
    }
    auto *thread = QThread::create([this, net = network(), address = address.toStdString()]() {
        auto result = ::electrum_tip_height(rust::String(address), true);
        emit tipReady(net, result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

auto CreatePolicy::spec() const -> PolicySpec {
    PolicySpec out;
    out.network = network();
    for (const auto &path : m_paths) {
        out.paths.push_back(policyPath(path, network(), m_tip));
    }
    return out;
}

auto CreatePolicy::accountPath() const -> QString {
    // The Internal role's xpub, R 0: the one every key of a path signs with.
    return QString::fromStdString(std::string(::bip87_account_path(network(), 0))) +
           QStringLiteral("/0");
}

auto CreatePolicy::missingXpubs() const -> QStringList {
    QStringList missing;
    for (const auto &fingerprint : m_fingerprints) {
        if (!m_account_xpubs.contains(fingerprint)) {
            missing.append(fingerprint);
        }
    }
    return missing;
}

void CreatePolicy::compilePolicy() {
    auto policy = spec();
    for (auto &path : policy.paths) {
        for (auto &key : path.keys) {
            auto fingerprint = QString::fromStdString(std::string(key.fingerprint));
            key.role_xpub = rust::String(m_account_xpubs.value(fingerprint).toStdString());
        }
    }
    auto result = ::compile_policy(policy);
    if (!result.is_ok) {
        showPaths();
        showStatus(compileError(result), catalog::LabelTone::Error);
        return;
    }
    auto descriptor = QString::fromStdString(std::string(result.descriptor));
    auto review = ::review_policy(rust::String(descriptor.toStdString()));
    if (!review.is_ok) {
        showPaths();
        showStatus(QString::fromStdString(std::string(review.error)), catalog::LabelTone::Error);
        return;
    }
    // Another descriptor: its backup, registrations and checks start over.
    if (descriptor != m_descriptor) {
        m_descriptor = descriptor;
        m_backed_up = false;
        m_registered.clear();
        m_register_failed.clear();
        m_verified.clear();
        m_verify_failed.clear();
        m_mismatched.clear();
        dropAsks(Ask::Register);
        dropAsks(Ask::Verify);
        m_skip_check->setChecked(false);
        m_address =
            ::musig_first_receive_address(rust::String(m_descriptor.toStdString()), network());
    }
    showReview();
}

auto CreatePolicy::needsRegister(const QString &fingerprint) const -> bool {
    auto entry = keyEntry(fingerprint);
    if (!entry.has_value()) {
        return false;
    }
    switch (entry->route) {
    case KeyRouteKind::Usb:
    case KeyRouteKind::Qr:
    case KeyRouteKind::SdCard:
        return true;
    case KeyRouteKind::Hot:
    case KeyRouteKind::Unknown:
    default:
        return false;
    }
}

void CreatePolicy::showStatus(const QString &text, catalog::LabelTone tone) {
    m_status_label->setText(text);
    m_status_label->setTone(tone);
    m_status_label->setVisible(!text.isEmpty());
}

void CreatePolicy::runFlow() {
    auto flow = m_pending_flow;
    m_pending_flow = Flow::None;
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return;
    }
    switch (flow) {
    case Flow::None:
        break;
    case Flow::Xpub: {
        auto req = beginAsk(Ask::Xpub, m_flow_fingerprint);
        // Returns once the flow's window has closed, its answer delivered.
        keys->xpub(req, network(), m_flow_fingerprint, accountPath());
        showXpubs();
        break;
    }
    case Flow::Verify: {
        auto req = beginAsk(Ask::Verify, m_flow_fingerprint);
        // Returns once the flow's window has closed, its answer delivered.
        keys->verifyAddress(
            req, network(), QString::fromStdString(std::string(m_address.descriptor)),
            QString::fromStdString(std::string(m_address.path)),
            QString::fromStdString(std::string(m_address.address)), m_flow_fingerprint);
        refreshVerifyRows();
        break;
    }
    }
}

auto CreatePolicy::beginAsk(Ask ask, const QString &fingerprint) -> ReqId {
    auto req = Host::get()->keys()->newRequest();
    m_asks.insert(req, {.ask = ask, .fingerprint = fingerprint});
    return req;
}

auto CreatePolicy::busy(const QString &fingerprint) const -> bool {
    return std::ranges::any_of(
        m_asks, [&fingerprint](const KeyAsk &a) { return a.fingerprint == fingerprint; });
}

auto CreatePolicy::asking(Ask ask, const QString &fingerprint) const -> bool {
    return std::ranges::any_of(m_asks, [ask, &fingerprint](const KeyAsk &a) {
        return !a.stale && a.ask == ask && a.fingerprint == fingerprint;
    });
}

auto CreatePolicy::askingAny(Ask ask) const -> bool {
    return std::ranges::any_of(m_asks, [ask](const KeyAsk &a) { return !a.stale && a.ask == ask; });
}

void CreatePolicy::dropAsks(Ask ask) {
    for (auto &a : m_asks) {
        if (a.ask == ask) {
            a.stale = true;
        }
    }
}

void CreatePolicy::markFailed(catalog::SelectRow *row, const QString &why) {
    row->setSub(why);
    row->setTone(catalog::SelectRow::Tone::Error);
}

// ===== Slots =====

void CreatePolicy::onBackClicked() {
    switch (m_view) {
    case View::Paths:
        break;
    case View::EditPath:
        showPaths();
        break;
    case View::Xpubs:
        showPaths();
        break;
    case View::Review:
        if (m_skipped_xpubs) {
            showPaths();
        } else {
            showXpubs();
        }
        break;
    case View::Register:
        if (!askingAny(Ask::Register)) {
            showReview();
        }
        break;
    case View::Verify:
        showRegister();
        break;
    case View::Electrum:
        showVerify();
        break;
    }
}

void CreatePolicy::onNextClicked() {
    switch (m_view) {
    case View::Paths: {
        if (!pathsReason().isEmpty()) {
            break;
        }
        m_fingerprints.clear();
        for (const auto &path : m_paths) {
            for (const auto &fingerprint : path.keys) {
                if (!m_fingerprints.contains(fingerprint)) {
                    m_fingerprints.append(fingerprint);
                }
            }
        }
        m_account_xpubs.clear();
        if (auto *keys = Host::get()->keys(); keys != nullptr) {
            for (const auto &fingerprint : m_fingerprints) {
                auto xpub = keys->cachedXpub(network(), fingerprint, accountPath());
                if (!xpub.isEmpty()) {
                    m_account_xpubs.insert(fingerprint, xpub);
                }
            }
        }
        // Every xpub cached: nothing to fetch, and Back from Review returns here.
        m_skipped_xpubs = missingXpubs().isEmpty();
        if (m_skipped_xpubs) {
            compilePolicy();
        } else {
            showXpubs();
        }
        break;
    }
    case View::EditPath: {
        auto path = m_editor->path();
        if (m_editing >= m_paths.size()) {
            m_paths.append(path);
        } else {
            m_paths[m_editing] = path;
        }
        showPaths();
        break;
    }
    case View::Xpubs:
        if (missingXpubs().isEmpty()) {
            compilePolicy();
        }
        break;
    case View::Review:
        showRegister();
        break;
    case View::Register:
        showVerify();
        break;
    case View::Verify:
        showElectrum();
        break;
    case View::Electrum:
        if (m_install_btn->isEnabled()) {
            install();
        }
        break;
    }
}

void CreatePolicy::onNetworkChanged([[maybe_unused]] int index) {
    // The paths pick keys of the network: start over from one path over its keys.
    m_paths.clear();
    m_paths.append(defaultPath());
    m_descriptor.clear();
    // The keys of another network: what is asked of them now is of no use.
    dropAsks(Ask::Xpub);
    dropAsks(Ask::Register);
    dropAsks(Ask::Verify);
    m_xpub_failed.clear();
    // The editor lists the network's keys.
    auto *editor = new PathEditor(networkKeys());
    connect(editor, &PathEditor::edited, this, &CreatePolicy::onPathEdited, qontrol::UNIQUE);
    // The column deletes the old editor.
    if (auto *column = qobject_cast<qontrol::Column *>(m_editor->parentWidget());
        column != nullptr) {
        column->replace(m_editor, editor);
    }
    m_editor = editor;
    applyNetworkDefaults();
    fetchTip();
    showPaths();
}

void CreatePolicy::onPathRowClicked() {
    showEditPath(sender()->property(K_INDEX).toInt());
}

void CreatePolicy::onAddPathClicked() {
    showEditPath(static_cast<int>(m_paths.size()));
}

void CreatePolicy::onRemovePathClicked() {
    if (m_editing < m_paths.size() && m_paths.size() > 1) {
        m_paths.removeAt(m_editing);
    }
    showPaths();
}

void CreatePolicy::onPathEdited() {
    refreshEditActions();
}

void CreatePolicy::onXpubRowClicked() {
    auto fingerprint = sender()->property(K_FINGERPRINT).toString();
    auto *keys = Host::get()->keys();
    if (keys == nullptr || busy(fingerprint)) {
        return;
    }
    m_xpub_failed.remove(fingerprint);
    // A connected USB device gives it with no window, while the others are asked too.
    if (auto entry = keyEntry(fingerprint); entry.has_value() && entry->on_device) {
        auto req = beginAsk(Ask::Xpub, fingerprint);
        keys->xpubOnDevice(req, network(), fingerprint, accountPath());
        refreshXpubRows();
        return;
    }
    m_flow_fingerprint = fingerprint;
    m_pending_flow = Flow::Xpub;
    done(FLOW_RESULT);
}

void CreatePolicy::onNameEdited([[maybe_unused]] const QString &text) {
    // The backup is named after the account: a new name saves it again.
    m_backed_up = false;
    refreshReviewActions();
}

void CreatePolicy::onBackupClicked() {
    auto name = m_name_input->text().trimmed();
    auto path = QFileDialog::getSaveFileName(
        this, TR("btn-backup-descriptor"),
        QDir::home().filePath(TR("musig-review-backup-file").arg(name)),
        TR("musig-review-backup-filter"));
    if (path.isEmpty()) {
        return;
    }
    // BIP138: the descriptor encrypted to its keys, any one of their xpubs decrypts it.
    auto descriptor = m_descriptor.toStdString();
    auto file = path.toStdString();
    auto written = ::save_descriptor_backup(rust::Str(descriptor), rust::Str(file),
                                            !m_privacy_check->isChecked());
    if (!written.is_ok) {
        showStatus(TR("musig-review-backup-failed")
                       .arg(path, QString::fromStdString(std::string(written.error))),
                   catalog::LabelTone::Error);
        return;
    }
    // The BIP asks that the user knows which key cannot decrypt the backup.
    QStringList excluded;
    for (const auto &key : written.excluded) {
        excluded.append(QString::fromStdString(std::string(key)));
    }
    m_backup_note->setText(
        TR("musig-review-backup-excluded").arg(excluded.join(QStringLiteral(", "))));
    m_backup_note->setVisible(!excluded.isEmpty());
    m_backed_up = true;
    refreshReviewActions();
}

void CreatePolicy::onRegisterRowClicked() {
    auto fingerprint = sender()->property(K_FINGERPRINT).toString();
    auto *keys = Host::get()->keys();
    if (keys == nullptr || busy(fingerprint)) {
        return;
    }
    m_register_failed.remove(fingerprint);
    auto req = beginAsk(Ask::Register, fingerprint);
    showStatus({}, catalog::LabelTone::Muted);
    refreshRegisterRows();
    // No window: the answer comes once the key's device has registered it, and may
    // come during the call.
    keys->registerDescriptor(req, network(), fingerprint, m_descriptor);
}

void CreatePolicy::onSkipToggled([[maybe_unused]] bool checked) {
    refreshRegisterRows();
}

void CreatePolicy::onVerifyRowClicked() {
    auto fingerprint = sender()->property(K_FINGERPRINT).toString();
    auto *keys = Host::get()->keys();
    if (keys == nullptr || busy(fingerprint)) {
        return;
    }
    m_verify_failed.remove(fingerprint);
    m_mismatched.remove(fingerprint);
    showStatus({}, catalog::LabelTone::Muted);
    // A connected USB device shows it with no window, while the others are asked too.
    if (auto entry = keyEntry(fingerprint); entry.has_value() && entry->on_device) {
        auto req = beginAsk(Ask::Verify, fingerprint);
        keys->verifyOnDevice(req, network(),
                             QString::fromStdString(std::string(m_address.descriptor)),
                             QString::fromStdString(std::string(m_address.path)),
                             QString::fromStdString(std::string(m_address.address)), fingerprint);
        refreshVerifyRows();
        return;
    }
    // The key's own flow, no signer picker: the click picked it.
    m_flow_fingerprint = fingerprint;
    m_pending_flow = Flow::Verify;
    done(FLOW_RESULT);
}

void CreatePolicy::onElectrumEdited() {
    m_electrum.setState(catalog::EndpointRow::State::Untested);
    refreshElectrumActions();
}

void CreatePolicy::onSslToggled([[maybe_unused]] bool checked) {
    // Another scheme: the server is tested again under it.
    auto bare = m_electrum.input()->text().trimmed();
    if (bare.contains(QStringLiteral("://"))) {
        bare = bare.section(QStringLiteral("://"), 1);
    }
    m_electrum.input()->setText((checked ? K_SSL_SCHEME : K_TCP_SCHEME) + bare);
    m_electrum.setState(catalog::EndpointRow::State::Untested);
    refreshElectrumActions();
}

void CreatePolicy::onVerifyCertificateToggled([[maybe_unused]] bool checked) {
    m_electrum.setState(catalog::EndpointRow::State::Untested);
    refreshElectrumActions();
}

void CreatePolicy::onTestElectrum() {
    auto address = electrumAddress();
    if (address.isEmpty()) {
        return;
    }
    m_electrum.setState(catalog::EndpointRow::State::Testing);
    refreshElectrumActions();
    bool verify = !m_ssl_toggle->isChecked() || m_verify_toggle->isChecked();
    auto *thread = QThread::create([this, address = address.toStdString(), verify]() {
        auto result = ::test_electrum(rust::String(address), verify);
        emit electrumTestReady(result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void CreatePolicy::onElectrumTestReady(const ConnectionResult &result) {
    // An edit or a network change since the test started voids its answer.
    if (m_electrum.state() != catalog::EndpointRow::State::Testing) {
        return;
    }
    if (!result.is_ok) {
        auto rawError = QString::fromStdString(std::string(result.error));
        m_electrum.setState(catalog::EndpointRow::State::Failed, mapBackendErrorSummary(rawError));
    } else {
        auto url = QString::fromStdString(std::string(result.url));
        m_electrum.input()->setText(url);
        {
            // The scheme that answered: no second test.
            QSignalBlocker blocker(m_ssl_toggle);
            m_ssl_toggle->setChecked(url.startsWith(K_SSL_SCHEME));
        }
        m_electrum.setState(catalog::EndpointRow::State::Ok);
    }
    refreshElectrumActions();
}

void CreatePolicy::onTipReady(Network network, const ElectrumTipResult &result) {
    // A network change since the fetch started voids its answer.
    if (network != this->network() || !result.is_ok) {
        return;
    }
    m_tip = result.height;
    m_editor->setTip(m_tip);
    switch (m_view) {
    case View::Paths:
        showPaths();
        break;
    case View::EditPath:
        refreshEditActions();
        break;
    case View::Xpubs:
    case View::Review:
    case View::Register:
    case View::Verify:
    case View::Electrum:
        break;
    }
}

void CreatePolicy::onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                               const QString &path) {
    auto found = m_asks.constFind(req_id);
    if (found == m_asks.cend() || found->ask != Ask::Xpub) {
        return;
    }
    auto ask = m_asks.take(req_id);
    if (!ask.stale) {
        // The request was for the key clicked, at the account path.
        if (fingerprint != ask.fingerprint || path != accountPath()) {
            m_xpub_failed.insert(ask.fingerprint, TR("keys-xpub-other-key"));
        } else {
            m_account_xpubs.insert(fingerprint, xpub);
        }
    }
    refreshKeyRows();
}

void CreatePolicy::onDescriptorRegistered(ReqId req_id, const QString &fingerprint) {
    auto found = m_asks.constFind(req_id);
    if (found == m_asks.cend() || found->ask != Ask::Register) {
        return;
    }
    auto ask = m_asks.take(req_id);
    if (!ask.stale) {
        m_registered.insert(fingerprint.isEmpty() ? ask.fingerprint : fingerprint);
    }
    refreshKeyRows();
    if (m_view == View::Register) {
        registerNextQuiet();
    }
}

void CreatePolicy::onAddressVerified(ReqId req_id, bool verified) {
    auto found = m_asks.constFind(req_id);
    if (found == m_asks.cend() || found->ask != Ask::Verify) {
        return;
    }
    auto ask = m_asks.take(req_id);
    if (!ask.stale) {
        if (verified) {
            m_verified.insert(ask.fingerprint);
        } else {
            m_mismatched.insert(ask.fingerprint);
            m_verify_failed.insert(ask.fingerprint, TR("musig-receive-verify-mismatch"));
        }
    }
    refreshKeyRows();
}

void CreatePolicy::onKeysError(std::optional<ReqId> req_id, const QString &message,
                               [[maybe_unused]] plugin::SignError kind) {
    if (!req_id.has_value() || !m_asks.contains(req_id.value())) {
        return;
    }
    auto ask = m_asks.take(req_id.value());
    // A failure shows on its key's row, not above the step.
    if (!ask.stale) {
        switch (ask.ask) {
        case Ask::Xpub:
            m_xpub_failed.insert(ask.fingerprint, message);
            break;
        case Ask::Register:
            m_register_failed.insert(ask.fingerprint, message);
            break;
        case Ask::Verify:
            m_verify_failed.insert(ask.fingerprint, message);
            break;
        }
    }
    refreshKeyRows();
}

void CreatePolicy::onKeysCancelled(ReqId req_id) {
    if (!m_asks.contains(req_id)) {
        return;
    }
    m_asks.remove(req_id);
    refreshKeyRows();
}

void CreatePolicy::refreshKeyRows() {
    switch (m_view) {
    case View::Xpubs:
        refreshXpubRows();
        break;
    case View::Register:
        refreshRegisterRows();
        break;
    case View::Verify:
        refreshVerifyRows();
        break;
    case View::Paths:
    case View::EditPath:
    case View::Review:
    case View::Electrum:
        break;
    }
}

} // namespace musig::modal
