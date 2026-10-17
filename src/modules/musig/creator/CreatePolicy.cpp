#include "modules/musig/creator/CreatePolicy.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/keys/KeysView.h"
#include "modules/musig/MusigModule.h"
#include "modules/musig/creator/PolicyEditor.h"
#include "modules/musig/creator/ReviewPolicy.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <Qontrol>
#include <algorithm>
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

auto networkKeys(Network network) -> QList<KeyEntry> {
    auto *bus = Host::get()->keys();
    return bus != nullptr ? bus->keys(network) : QList<KeyEntry>{};
}

// Fingerprint to the name a key is shown by: its alias, else its fingerprint.
auto keyNames(Network network) -> QHash<QString, QString> {
    QHash<QString, QString> names;
    for (const auto &entry : networkKeys(network)) {
        names.insert(entry.fingerprint, entry.alias.isEmpty() ? entry.fingerprint : entry.alias);
    }
    return names;
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

} // namespace

CreatePolicy::CreatePolicy() {
    init();
    doConnect();
    view();
    applyNetworkDefaults();
    previewPolicy();
    updateActions();
    adjustSize();
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
    m_name_input = new catalog::Input;
    m_name_input->setWidth(Size::M);
    m_name_input->setPlaceholderText(TR("create-account-name-placeholder"));

    m_network_combo = new catalog::ComboBox;
    m_network_combo->setWidth(Size::M);
    for (auto network : NETWORKS) {
        m_network_combo->addItem(keys::networkLabel(network), static_cast<int>(network));
    }

    m_editor = new PolicyEditor;
    m_editor->setNetwork(network());
    m_editor->setKeys(networkKeys(network()));

    m_preview_leaves = new catalog::Label(catalog::LabelRole::Body);
    m_preview_paths = new qontrol::Column;
    m_preview_paths->spacing(resolve(Spacing::XS));
    m_preview_error = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Error);
    m_preview_error->setVisible(false);
    m_preview_card = new catalog::Card(catalog::Card::Role::Inset);

    m_status_label = new catalog::Label(catalog::LabelRole::Caption);
    m_status_label->setVisible(false);

    m_next_btn = catalog::btnContinue();
    m_fetch_btn = catalog::btnFetchThisXpub();
    m_register_btn = catalog::btnRegister();
    m_verify_btn = catalog::btnCheckAddress();
    m_install_btn = catalog::btnCreate();
    m_actions = new catalog::ModalActions;
    for (auto *button : {m_next_btn, m_fetch_btn, m_register_btn, m_install_btn}) {
        m_actions->addAction(button);
    }

    m_missing_column = new qontrol::Column;

    m_review = new ReviewPolicy;

    m_verify_lead = new catalog::Label(TR("musig-create-verify-lead"), catalog::LabelRole::Body);
    m_verify_lead->setWordWrap(true);
    m_address_label = new catalog::ChunkedAddress;
    m_verify_status = new catalog::Label(catalog::LabelRole::Caption);
    m_verify_status->setVisible(false);
    m_skip_notice = new catalog::Notice(catalog::Notice::Tone::Warning);
    m_skip_notice->setTitle(TR("musig-create-skip-verify-title"));
    m_skip_notice->setText(TR("musig-create-skip-verify"));
}

void CreatePolicy::doConnect() {
    connect(m_name_input, &QLineEdit::textChanged, this, &CreatePolicy::onNameEdited,
            qontrol::UNIQUE);
    connect(m_network_combo, &QComboBox::currentIndexChanged, this, &CreatePolicy::onNetworkChanged,
            qontrol::UNIQUE);
    connect(m_editor, &PolicyEditor::edited, this, &CreatePolicy::onPolicyEdited, qontrol::UNIQUE);
    connect(m_electrum.testButton(), &QPushButton::clicked, this, &CreatePolicy::onTestElectrum,
            qontrol::UNIQUE);
    connect(m_electrum.input(), &QLineEdit::textEdited, this, &CreatePolicy::onElectrumEdited,
            qontrol::UNIQUE);
    connect(this, &CreatePolicy::electrumTestReady, this, &CreatePolicy::onElectrumTestReady,
            qontrol::UNIQUE);
    for (auto *button : {m_next_btn, m_fetch_btn, m_register_btn, m_install_btn}) {
        connect(button, &QPushButton::clicked, this, &CreatePolicy::onNextClicked, qontrol::UNIQUE);
    }
    connect(m_verify_btn, &QPushButton::clicked, this, &CreatePolicy::onCheckAddressClicked,
            qontrol::UNIQUE);
    connect(m_actions, &catalog::ModalActions::backClicked, this, &CreatePolicy::onBackClicked,
            qontrol::UNIQUE);

    auto *bus = Host::get()->keys();
    if (bus != nullptr) {
        connect(bus, &IKeys::xpubReady, this, &CreatePolicy::onXpubReady, qontrol::UNIQUE);
        connect(bus, &IKeys::descriptorRegistered, this, &CreatePolicy::onDescriptorRegistered,
                qontrol::UNIQUE);
        connect(bus, &IKeys::verified, this, &CreatePolicy::onAddressVerified, qontrol::UNIQUE);
        connect(bus, &IKeys::error, this, &CreatePolicy::onKeysError, qontrol::UNIQUE);
        connect(bus, &IKeys::cancelled, this, &CreatePolicy::onKeysCancelled, qontrol::UNIQUE);
    }
}

void CreatePolicy::view() {
    auto *account = (new catalog::FieldGroup)
                        ->addField(TR("create-account-name"), m_name_input)
                        ->addField(TR("settings-network"), m_network_combo)
                        ->addField(TR("create-account-electrum"), m_electrum.control())
                        ->addNote(m_electrum.note());

    auto *previewLabel =
        new catalog::Label(TR("musig-create-preview"), catalog::LabelRole::Eyebrow);
    previewLabel->setTone(catalog::LabelTone::Muted);
    auto *preview = (new qontrol::Column)
                        ->spacing(resolve(Spacing::XS))
                        ->push(previewLabel)
                        ->push(m_preview_leaves)
                        ->push(m_preview_paths)
                        ->push(m_preview_error);
    m_preview_card->setContent(preview);

    auto *body = (new qontrol::Column)->spacing(resolve(Spacing::M))->push(account)->push(m_editor);
    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    scroll->setWidget(body);
    // The preview stays in sight under the scrolling paths.
    m_define_page =
        (new qontrol::Column)->spacing(resolve(Spacing::M))->push(scroll)->push(m_preview_card);

    m_footer =
        (new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_status_label)->push(m_actions);

    m_fetch_page = (new qontrol::Column)
                       ->spacing(resolve(Spacing::M))
                       ->push(new catalog::Label(TR("musig-create-fetch-lead")))
                       ->push(m_missing_column);
    keep(m_fetch_page);

    auto *reviewScroll = new catalog::ScrollArea;
    reviewScroll->setFitContent(true);
    reviewScroll->setWidget(m_review);
    m_review_page = reviewScroll;
    keep(m_review_page);

    m_verify_page = (new qontrol::Column)
                        ->spacing(resolve(Spacing::M))
                        ->push(m_verify_lead)
                        ->push(m_address_label)
                        ->push((new qontrol::Row)->push(m_verify_btn)->pushSpacer())
                        ->push(m_verify_status)
                        ->push(m_skip_notice);
    keep(m_verify_page);

    setWindowTitle(TR("musig-create-title"));
    setWidth(ModalWidth::XL);
    showStep(Step::Define);
}

void CreatePolicy::showStep(Step step) {
    m_step = step;
    switch (step) {
    case Step::Define:
    case Step::Compile:
        setBody(m_define_page);
        break;
    case Step::FetchXpubs: {
        auto names = keyNames(network());
        auto missing = missingXpubs();
        m_missing_column->clear();
        m_missing_column->spacing(resolve(Spacing::XS));
        auto *count = new catalog::Label(TR("musig-create-fetch-status")
                                             .arg(m_fingerprints.size() - missing.size())
                                             .arg(m_fingerprints.size()),
                                         catalog::LabelRole::Caption);
        count->setTone(catalog::LabelTone::Secondary);
        m_missing_column->push(count);
        count->show();
        for (const auto &fingerprint : missing) {
            auto *line =
                new catalog::Label(TR("musig-create-fetch-key")
                                       .arg(names.value(fingerprint, fingerprint), fingerprint));
            m_missing_column->push(line);
            line->show();
        }
        setBody(m_fetch_page);
        break;
    }
    case Step::Review:
    case Step::Register:
        setBody(m_review_page);
        break;
    case Step::VerifyAddress: {
        bool checkable = canCheckAddress();
        m_address_label->setAddress(QString::fromStdString(std::string(m_address.address)));
        m_verify_lead->setText(checkable ? TR("musig-create-verify-lead")
                                         : TR("musig-create-verify-lead-unchecked"));
        m_skip_notice->setVisible(!checkable);
        setBody(m_verify_page);
        break;
    }
    case Step::Install:
        break;
    }
    setFooter(m_footer);
    updateStepActions();
}

auto CreatePolicy::network() const -> Network {
    return static_cast<Network>(m_network_combo->currentData().toInt());
}

auto CreatePolicy::disabledReason() const -> QString {
    QString name = m_name_input->text();
    QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return TR("create-account-reason-name");
    }
    if (name.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return TR("create-account-reason-spaces");
    }
    if (accountExists(trimmed)) {
        return TR("create-account-reason-exists").arg(trimmed);
    }
    if (networkKeys(network()).isEmpty()) {
        return TR("musig-create-reason-keys");
    }
    if (m_electrum.state() != catalog::EndpointRow::State::Ok) {
        return TR("musig-create-reason-electrum");
    }
    if (!m_preview_ok) {
        return TR("musig-create-reason-policy");
    }
    return {};
}

void CreatePolicy::updateActions() {
    QString reason = disabledReason();
    m_next_btn->setEnabled(reason.isEmpty());
    m_actions->setReason(reason);
}

void CreatePolicy::updateStepActions() {
    bool define = m_step == Step::Define || m_step == Step::Compile;
    bool review = m_step == Step::Review || m_step == Step::Register;
    bool verify = m_step == Step::VerifyAddress;
    bool checkable = verify && canCheckAddress();
    m_next_btn->setVisible(define);
    m_fetch_btn->setVisible(m_step == Step::FetchXpubs);
    m_register_btn->setVisible(review);
    m_verify_btn->setVisible(checkable);
    m_install_btn->setVisible(verify);
    m_actions->setBack(define ? catalog::ModalActions::Back::None
                              : catalog::ModalActions::Back::Back);
    if (define) {
        updateActions();
        return;
    }

    QString reason;
    bool registering = m_step == Step::Register && m_req_id != 0 && !m_to_register.isEmpty();
    if (registering) {
        auto names = keyNames(network());
        reason = TR("musig-create-registering")
                     .arg(names.value(m_to_register.first(), m_to_register.first()));
    }
    bool installable = !checkable || m_address_verified;
    if (verify && !installable) {
        reason = TR("musig-create-reason-verify");
    }
    m_fetch_btn->setEnabled(m_req_id == 0);
    m_register_btn->setEnabled(!registering);
    m_verify_btn->setEnabled(m_req_id == 0);
    // Once the address matched, Create is the only primary action.
    m_verify_btn->setRole(m_address_verified ? catalog::ButtonRole::Default
                                             : catalog::ButtonRole::Primary);
    m_install_btn->setEnabled(installable);
    m_actions->setReason(reason);
}

void CreatePolicy::showStatus(const QString &text, catalog::LabelTone tone) {
    m_status_label->setText(text);
    m_status_label->setTone(tone);
    m_status_label->setVisible(!text.isEmpty());
}

void CreatePolicy::applyNetworkDefaults() {
    // The server the last account of this network was created with comes first.
    auto electrum = QString::fromStdString(
        std::string(::app_last_endpoint(BackendServer::Electrum, network())));
    if (electrum.isEmpty() && network() == Network::Regtest) {
        auto defaults = AppController::get()->regtestDefaults();
        if (defaults.has_value()) {
            electrum = defaults.value().electrum_url;
        }
    }
    m_electrum.input()->setText(electrum);
    m_electrum.setState(catalog::EndpointRow::State::Untested);
    // Test the default rather than assume it: only a passing test resolves the scheme
    // that gets saved.
    onTestElectrum();
}

void CreatePolicy::previewPolicy() {
    auto spec = m_editor->spec();
    auto result = ::preview_policy(spec);
    m_preview_ok = result.is_ok;

    QStringList leaves;
    // A key path only policy has no leaf to count.
    if (result.leaf_count > 0 || !result.has_key_path) {
        leaves.append(TR("musig-create-leaves").arg(result.leaf_count).arg(result.max_leaves));
    }
    if (result.has_key_path) {
        leaves.append(TR("musig-create-key-path"));
    }
    m_preview_leaves->setText(leaves.join(QLatin1Char('\n')));
    m_preview_leaves->setVisible(result.is_ok);

    auto names = keyNames(network());
    m_preview_paths->clear();
    bool anyKey = false;
    for (std::size_t i = 0; i < spec.paths.size(); ++i) {
        const auto &path = spec.paths[i];
        if (path.keys.empty()) {
            continue;
        }
        anyKey = true;
        QStringList signers;
        QList<bool> mandatory;
        for (const auto &key : path.keys) {
            auto fingerprint = QString::fromStdString(std::string(key.fingerprint));
            signers.append(names.value(fingerprint, fingerprint));
            mandatory.append(key.mandatory);
        }
        auto words = pathWords(path.semantic, path.threshold, path.locktime, path.locktime_value,
                               signers, mandatory);
        auto *line = new catalog::Label(
            TR("musig-create-path-words").arg(static_cast<int>(i) + 1).arg(words));
        line->setWordWrap(true);
        m_preview_paths->push(line);
        // A label added to a shown column stays hidden until the event loop shows it.
        line->show();
    }

    // Before any key is picked, a hint reads better than the compiler error.
    auto tone = anyKey ? catalog::FieldNote::Tone::Error : catalog::FieldNote::Tone::Muted;
    m_preview_error->setTone(tone);
    m_preview_error->setText(anyKey ? compileError(result) : TR("musig-create-pick-keys"));
    m_preview_error->setVisible(!result.is_ok);
    updateActions();
}

auto CreatePolicy::accountPath() const -> QString {
    return QString::fromStdString(std::string(::bip87_account_path(network(), 0)));
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

void CreatePolicy::fetchNextXpub() {
    auto *keys = Host::get()->keys();
    auto missing = missingXpubs();
    if (keys == nullptr || missing.isEmpty()) {
        return;
    }
    m_req_id = keys->newRequest();
    // Returns once the flow's window has closed, its answer delivered.
    keys->xpub(m_req_id, network(), missing.first(), accountPath());
}

void CreatePolicy::compilePolicy() {
    for (auto &path : m_spec.paths) {
        for (auto &key : path.keys) {
            auto fingerprint = QString::fromStdString(std::string(key.fingerprint));
            key.account_xpub = rust::String(m_account_xpubs.value(fingerprint).toStdString());
        }
    }
    auto result = ::compile_policy(m_spec);
    if (!result.is_ok) {
        m_preview_error->setTone(catalog::FieldNote::Tone::Error);
        m_preview_error->setText(compileError(result));
        m_preview_error->setVisible(true);
        showStep(Step::Define);
        return;
    }
    m_descriptor = QString::fromStdString(std::string(result.descriptor));
    showReview();
}

void CreatePolicy::showReview() {
    auto review = ::review_policy(rust::String(m_descriptor.toStdString()));
    if (!review.is_ok) {
        m_preview_error->setTone(catalog::FieldNote::Tone::Error);
        m_preview_error->setText(QString::fromStdString(std::string(review.error)));
        m_preview_error->setVisible(true);
        m_descriptor.clear();
        showStep(Step::Define);
        return;
    }
    m_review->setAliases(keyNames(network()));
    m_review->setReview(review, m_descriptor);
    showStep(Step::Review);
}

void CreatePolicy::registerNext() {
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return;
    }
    if (m_to_register.isEmpty()) {
        m_address =
            ::musig_first_receive_address(rust::String(m_descriptor.toStdString()), network());
        if (!m_address.is_ok) {
            showStatus(QString::fromStdString(std::string(m_address.error)),
                       catalog::LabelTone::Error);
            updateStepActions();
            return;
        }
        showStep(Step::VerifyAddress);
        return;
    }
    m_req_id = keys->newRequest();
    updateStepActions();
    // Recorded in place, no window: the answer arrives during the call.
    keys->registerDescriptor(m_req_id, network(), m_to_register.first(), m_descriptor);
}

auto CreatePolicy::canCheckAddress() const -> bool {
    return std::ranges::any_of(networkKeys(network()), [this](const KeyEntry &entry) {
        return m_fingerprints.contains(entry.fingerprint) && entry.can_show_address;
    });
}

void CreatePolicy::verifyAddress() {
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return;
    }
    m_req_id = keys->newRequest();
    // Returns once the flow's window has closed, its answer delivered.
    keys->verifyAddress(m_req_id, network(),
                        QString::fromStdString(std::string(m_address.descriptor)),
                        QString::fromStdString(std::string(m_address.path)),
                        QString::fromStdString(std::string(m_address.address)));
}

void CreatePolicy::install() {
    auto name = m_name_input->text().trimmed();
    if (accountExists(name)) {
        showStatus(TR("create-account-reason-exists").arg(name), catalog::LabelTone::Error);
        return;
    }
    auto electrumUrl = m_electrum.input()->text().trimmed();
    auto config = new_config(
        rust::String(name.toStdString()), network(), rust::String(m_descriptor.toStdString()),
        rust::String(), rust::String(electrumUrl.toStdString()), DUST_LIMIT, rust::String(MUSIG_PLUGIN_ID));
    config->to_file();
    // Prefilled for the next account of this network.
    ::app_set_last_endpoint(BackendServer::Electrum, network(),
                            rust::String(electrumUrl.toStdString()));
    Host::get()->refreshLauncher();
    accept();
}

void CreatePolicy::runFlow() {
    auto flow = m_pending_flow;
    m_pending_flow = Flow::None;
    switch (flow) {
    case Flow::None:
        break;
    case Flow::Xpubs:
        // One window per key, in order, until a key gives none.
        while (!missingXpubs().isEmpty()) {
            auto asked = missingXpubs().first();
            fetchNextXpub();
            if (!m_account_xpubs.contains(asked)) {
                break;
            }
        }
        if (missingXpubs().isEmpty()) {
            compilePolicy();
        } else {
            showStep(Step::FetchXpubs);
        }
        break;
    case Flow::Verify:
        verifyAddress();
        updateStepActions();
        break;
    }
}

// ===== Slots =====

void CreatePolicy::onNextClicked() {
    switch (m_step) {
    case Step::Define:
        if (!disabledReason().isEmpty()) {
            break;
        }
        m_step = Step::Compile;
        previewPolicy();
        if (!m_preview_ok) {
            showStep(Step::Define);
            break;
        }
        m_spec = m_editor->spec();
        m_fingerprints.clear();
        for (const auto &path : m_spec.paths) {
            for (const auto &key : path.keys) {
                auto fingerprint = QString::fromStdString(std::string(key.fingerprint));
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
        // Every xpub cached: no window to open.
        if (missingXpubs().isEmpty()) {
            compilePolicy();
        } else {
            showStep(Step::FetchXpubs);
        }
        break;
    case Step::Compile:
        break;
    case Step::FetchXpubs:
        showStatus({}, catalog::LabelTone::Muted);
        m_pending_flow = Flow::Xpubs;
        done(FLOW_RESULT);
        break;
    case Step::Review:
        showStatus({}, catalog::LabelTone::Muted);
        m_to_register = m_fingerprints;
        showStep(Step::Register);
        registerNext();
        break;
    case Step::Register:
        // Registers what an error left.
        showStatus({}, catalog::LabelTone::Muted);
        registerNext();
        break;
    case Step::VerifyAddress:
        if (m_install_btn->isEnabled()) {
            install();
        }
        break;
    case Step::Install:
        break;
    }
}

void CreatePolicy::onBackClicked() {
    switch (m_step) {
    case Step::Define:
        break;
    case Step::Compile:
    case Step::FetchXpubs:
    case Step::Review:
    case Step::Register:
    case Step::VerifyAddress:
    case Step::Install:
        showStatus({}, catalog::LabelTone::Muted);
        m_descriptor.clear();
        m_address = MusigReceiveAddress{};
        m_address_verified = false;
        m_verify_status->setVisible(false);
        m_to_register.clear();
        showStep(Step::Define);
        break;
    }
}

void CreatePolicy::onNetworkChanged([[maybe_unused]] int index) {
    m_editor->setNetwork(network());
    // Fresh paths over the network's keys, previewed through PolicyEditor::edited.
    m_editor->setKeys(networkKeys(network()));
    applyNetworkDefaults();
    updateActions();
    adjustSize();
}

void CreatePolicy::onNameEdited([[maybe_unused]] const QString &text) {
    updateActions();
}

void CreatePolicy::onPolicyEdited() {
    previewPolicy();
    adjustSize();
}

void CreatePolicy::onElectrumEdited() {
    m_electrum.setState(catalog::EndpointRow::State::Untested);
    updateActions();
    adjustSize();
}

void CreatePolicy::onTestElectrum() {
    auto address = m_electrum.input()->text().trimmed();
    if (address.isEmpty()) {
        return;
    }
    m_electrum.setState(catalog::EndpointRow::State::Testing);
    updateActions();
    auto *thread = QThread::create([this, address = address.toStdString()]() {
        auto result = ::test_electrum(rust::String(address));
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
        m_electrum.input()->setText(QString::fromStdString(std::string(result.url)));
        m_electrum.setState(catalog::EndpointRow::State::Ok);
    }
    updateActions();
    adjustSize();
}

void CreatePolicy::onCheckAddressClicked() {
    if (m_step != Step::VerifyAddress) {
        return;
    }
    showStatus({}, catalog::LabelTone::Muted);
    m_verify_status->setVisible(false);
    m_pending_flow = Flow::Verify;
    done(FLOW_RESULT);
}

void CreatePolicy::onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                               const QString &path) {
    if (req_id != m_req_id) {
        return;
    }
    m_req_id = 0;
    // The request was for the first key still missing, at the account path.
    auto missing = missingXpubs();
    if (missing.isEmpty() || fingerprint != missing.first() || path != accountPath()) {
        showStatus(TR("keys-xpub-other-key"), catalog::LabelTone::Error);
        return;
    }
    m_account_xpubs.insert(fingerprint, xpub);
}

void CreatePolicy::onDescriptorRegistered(ReqId req_id,
                                          [[maybe_unused]] const QString &fingerprint) {
    if (req_id != m_req_id) {
        return;
    }
    m_req_id = 0;
    m_to_register.removeFirst();
    registerNext();
}

void CreatePolicy::onAddressVerified(ReqId req_id, bool verified) {
    if (req_id != m_req_id) {
        return;
    }
    m_req_id = 0;
    m_address_verified = verified;
    m_verify_status->setText(verified ? TR("musig-create-verify-match")
                                      : TR("musig-receive-verify-mismatch"));
    m_verify_status->setTone(verified ? catalog::LabelTone::Success : catalog::LabelTone::Error);
    m_verify_status->setVisible(true);
    updateStepActions();
}

void CreatePolicy::onKeysError(std::optional<ReqId> req_id, const QString &message,
                               [[maybe_unused]] plugin::SignError kind) {
    if (!req_id.has_value() || req_id.value() != m_req_id) {
        return;
    }
    m_req_id = 0;
    showStatus(message, catalog::LabelTone::Error);
    updateStepActions();
}

void CreatePolicy::onKeysCancelled(ReqId req_id) {
    if (req_id != m_req_id) {
        return;
    }
    m_req_id = 0;
    updateStepActions();
}

} // namespace musig::modal
