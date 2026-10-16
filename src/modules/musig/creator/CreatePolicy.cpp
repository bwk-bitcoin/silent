#include "modules/musig/creator/CreatePolicy.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/display/Label.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/keys/KeysView.h"
#include "modules/musig/creator/PolicyEditor.h"
#include "modules/musig/creator/ReviewPolicy.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <Qontrol>
#include <array>
#include <common.h>
#include <string>

namespace musig::modal {

namespace {

constexpr std::array<Network, 4> NETWORKS = {Network::Regtest, Network::Signet, Network::Testnet,
                                             Network::Bitcoin};

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
    return catalog::Modal::exec();
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
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_next_btn);

    m_review = new ReviewPolicy;
    m_review->setVisible(false);
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
    connect(m_next_btn, &QPushButton::clicked, this, &CreatePolicy::onNextClicked, qontrol::UNIQUE);
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

    keep(m_status_label);
    keep(m_review);

    setWindowTitle(TR("musig-create-title"));
    setWidth(ModalWidth::XL);
    showStep(Step::Define);
}

void CreatePolicy::showStep(Step step) {
    m_step = step;
    switch (step) {
    case Step::Define:
        setBody(m_define_page);
        setFooter(m_actions);
        m_actions->setBack(catalog::ModalActions::Back::None);
        break;
    case Step::Compile:
    case Step::FetchXpubs:
    case Step::Review:
    case Step::Register:
    case Step::VerifyAddress:
    case Step::Install:
        break;
    }
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

    QHash<QString, QString> names;
    for (const auto &entry : networkKeys(network())) {
        names.insert(entry.fingerprint, entry.alias.isEmpty() ? entry.fingerprint : entry.alias);
    }
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
    QString error;
    if (!anyKey) {
        error = TR("musig-create-pick-keys");
    } else if (result.too_many_leaves) {
        error = TR("musig-create-too-many-leaves").arg(result.leaf_count).arg(result.max_leaves);
    } else if (result.error_path >= 0) {
        error = TR("musig-create-path-error")
                    .arg(result.error_path + 1)
                    .arg(QString::fromStdString(std::string(result.error)));
    } else {
        error = QString::fromStdString(std::string(result.error));
    }
    m_preview_error->setTone(tone);
    m_preview_error->setText(error);
    m_preview_error->setVisible(!result.is_ok);
    updateActions();
}

void CreatePolicy::fetchNextXpub() {
}

void CreatePolicy::compilePolicy() {
}

void CreatePolicy::showReview() {
}

void CreatePolicy::registerNext() {
}

void CreatePolicy::verifyAddress() {
}

void CreatePolicy::install() {
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
        showStep(Step::FetchXpubs);
        break;
    case Step::Compile:
    case Step::FetchXpubs:
    case Step::Review:
    case Step::Register:
    case Step::VerifyAddress:
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

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void CreatePolicy::onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                               const QString &path) {
    Q_UNUSED(req_id);
    Q_UNUSED(xpub);
    Q_UNUSED(fingerprint);
    Q_UNUSED(path);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void CreatePolicy::onDescriptorRegistered(ReqId req_id, const QString &fingerprint) {
    Q_UNUSED(req_id);
    Q_UNUSED(fingerprint);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void CreatePolicy::onAddressVerified(ReqId req_id, bool verified) {
    Q_UNUSED(req_id);
    Q_UNUSED(verified);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void CreatePolicy::onKeysError(std::optional<ReqId> req_id, const QString &message,
                               plugin::SignError kind) {
    Q_UNUSED(req_id);
    Q_UNUSED(message);
    Q_UNUSED(kind);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void CreatePolicy::onKeysCancelled(ReqId req_id) {
    Q_UNUSED(req_id);
}

} // namespace musig::modal
