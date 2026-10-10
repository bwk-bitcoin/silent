#include "CreateAccount.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/CopyField.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "modules/keys/KeysView.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <Qontrol>
#include <array>
#include <common.h>
#include <string>

namespace modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::ValidationMark;

namespace {

// Default endpoints for a new mainnet account. Both are prefilled without a scheme
// so the test resolves it: http/https for blindbit, tcp/ssl for electrum.
constexpr auto MAINNET_BLINDBIT_URL = "blindbit.pythcoiner.dev";
constexpr auto MAINNET_ELECTRUM_URL = "electrum.pythcoiner.dev:50002";
// Regtest BlindBit when the app found no local regtest setup.
constexpr auto REGTEST_BLINDBIT_URL = "http://localhost:8080";

// exec() result of a window closed to fetch the descriptor in its place.
constexpr int FETCH_RESULT = QDialog::Accepted + 1;

constexpr std::array<Network, 4> NETWORKS = {Network::Regtest, Network::Signet, Network::Testnet,
                                             Network::Bitcoin};

auto sectionLabel(const QString &text) -> Label * {
    auto *label = new Label(text, LabelRole::Eyebrow);
    label->setTone(catalog::LabelTone::Muted);
    return label;
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

} // namespace

EndpointRow::EndpointRow() {
    m_input = new catalog::Input(catalog::InputRole::Mono);
    m_test = catalog::btnTest();
    m_testing = catalog::btnTesting();
    m_testing->setEnabled(false);
    m_testing->setVisible(false);
    m_mark = new ValidationMark;
    m_note = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Error);
    m_note->setVisible(false);
    m_control = (new qontrol::Row)
                    ->spacing(resolve(Spacing::XS))
                    ->push(m_input, 1)
                    ->push(m_test)
                    ->push(m_testing)
                    ->push(m_mark);
}

auto EndpointRow::control() const -> QWidget * {
    return m_control;
}

auto EndpointRow::note() const -> catalog::FieldNote * {
    return m_note;
}

auto EndpointRow::input() const -> catalog::Input * {
    return m_input;
}

auto EndpointRow::testButton() const -> catalog::Button * {
    return m_test;
}

auto EndpointRow::state() const -> State {
    return m_state;
}

void EndpointRow::setState(State state, const QString &error) {
    m_state = state;
    bool testing = state == State::Testing;
    m_test->setVisible(!testing);
    m_testing->setVisible(testing);
    m_test->setEnabled(!m_input->text().trimmed().isEmpty());
    switch (state) {
    case State::Ok:
        m_mark->setState(ValidationMark::State::Valid);
        break;
    case State::Failed:
        m_mark->setState(ValidationMark::State::Invalid);
        break;
    case State::Untested:
    case State::Testing:
        m_mark->setState(ValidationMark::State::None);
        break;
    }
    m_note->setText(error);
    m_note->setVisible(state == State::Failed && !error.isEmpty());
}

CreateAccount::CreateAccount() {
    init();
    doConnect();
    view();
    populateKeys();
    applyNetworkDefaults();
    updateActions();
    refit();
}

void CreateAccount::init() {
    m_name_input = new catalog::Input;
    m_name_input->setWidth(Size::M);
    m_name_input->setPlaceholderText(TR("create-account-name-placeholder"));

    m_network_combo = new catalog::ComboBox;
    m_network_combo->setWidth(Size::M);
    for (auto network : NETWORKS) {
        m_network_combo->addItem(keys::networkLabel(network), static_cast<int>(network));
    }

    m_keys_column = new qontrol::Column;

    m_get_descriptor = catalog::btnGetDescriptor();
    m_get_again = catalog::btnGetAgain();
    m_get_again->setVisible(false);
    m_pick_key = new Label(TR("create-account-pick-key"), LabelRole::Caption);
    m_fetch_row = (new qontrol::Row)
                      ->spacing(resolve(Spacing::S))
                      ->push(m_get_descriptor)
                      ->push(m_get_again)
                      ->push(m_pick_key)
                      ->pushSpacer();

    m_fetch_error = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Error);
    m_fetch_error->setVisible(false);

    m_descriptor_field = new catalog::CopyField;
    m_descriptor_field->setWrap(true);
    m_descriptor_field->setVisible(false);

    m_create = catalog::btnCreate();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_create);
}

void CreateAccount::doConnect() {
    connect(m_name_input, &QLineEdit::textChanged, this, &CreateAccount::updateActions,
            qontrol::UNIQUE);
    connect(m_network_combo, &QComboBox::currentIndexChanged, this,
            &CreateAccount::onNetworkChanged, qontrol::UNIQUE);
    connect(m_get_descriptor, &QPushButton::clicked, this, &CreateAccount::onFetchClicked,
            qontrol::UNIQUE);
    connect(m_get_again, &QPushButton::clicked, this, &CreateAccount::onFetchClicked,
            qontrol::UNIQUE);
    connect(m_blindbit.testButton(), &QPushButton::clicked, this, &CreateAccount::onTestBlindbit,
            qontrol::UNIQUE);
    connect(m_blindbit.input(), &QLineEdit::textEdited, this, &CreateAccount::onBlindbitEdited,
            qontrol::UNIQUE);
    connect(m_electrum.testButton(), &QPushButton::clicked, this, &CreateAccount::onTestElectrum,
            qontrol::UNIQUE);
    connect(m_electrum.input(), &QLineEdit::textEdited, this, &CreateAccount::onElectrumEdited,
            qontrol::UNIQUE);
    connect(m_create, &QPushButton::clicked, this, &CreateAccount::onCreate, qontrol::UNIQUE);
    connect(this, &CreateAccount::backendInfoReady, this, &CreateAccount::onBackendInfoReady,
            qontrol::UNIQUE);
    connect(this, &CreateAccount::electrumTestReady, this, &CreateAccount::onElectrumTestReady,
            qontrol::UNIQUE);

    auto *bus = Host::get()->keys();
    if (bus != nullptr) {
        connect(bus, &IKeys::spDescriptorReady, this, &CreateAccount::onSpDescriptorReady,
                qontrol::UNIQUE);
        connect(bus, &IKeys::descriptorRegistered, this, &CreateAccount::onDescriptorRegistered,
                qontrol::UNIQUE);
        connect(bus, &IKeys::error, this, &CreateAccount::onKeysError, qontrol::UNIQUE);
        connect(bus, &IKeys::cancelled, this, &CreateAccount::onKeysCancelled, qontrol::UNIQUE);
    }
}

void CreateAccount::view() {
    auto *account = (new catalog::FieldGroup)
                        ->addField(TR("create-account-name"), m_name_input)
                        ->addField(TR("settings-network"), m_network_combo);

    auto *key = (new qontrol::Column)
                    ->spacing(resolve(Spacing::S))
                    ->push(sectionLabel(TR("create-account-key-section")))
                    ->push(m_keys_column)
                    ->push(m_fetch_row)
                    ->push(m_fetch_error)
                    ->push(m_descriptor_field);

    auto *endpoints = (new catalog::FieldGroup)
                          ->addField(TR("settings-blindbit-url"), m_blindbit.control())
                          ->addNote(m_blindbit.note())
                          ->addField(TR("create-account-electrum"), m_electrum.control())
                          ->addNote(m_electrum.note());
    auto *backends = (new qontrol::Column)
                         ->spacing(resolve(Spacing::XS))
                         ->push(sectionLabel(TR("create-account-backends-section")))
                         ->push(endpoints);

    auto *body = (new qontrol::Column)
                     ->spacing(resolve(Spacing::M))
                     ->push(account)
                     ->push(key)
                     ->push(backends);
    m_step = addStep(TR("create-account-title"), ModalWidth::XL, body, m_actions);
    showStep(m_step);
}

auto CreateAccount::exec() -> int {
    int result = catalog::StepModal::exec();
    while (result == FETCH_RESULT) {
        fetchDescriptor();
        result = catalog::StepModal::exec();
    }
    return result;
}

auto CreateAccount::network() const -> Network {
    return static_cast<Network>(m_network_combo->currentData().toInt());
}

auto CreateAccount::disabledReason() const -> QString {
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
    if (m_descriptor.isEmpty()) {
        return TR("create-account-reason-descriptor");
    }
    if (m_descriptor_network != network()) {
        return TR("create-account-reason-descriptor-network")
            .arg(keys::networkLabel(m_descriptor_network), keys::networkLabel(network()));
    }
    if (m_blindbit.state() != EndpointRow::State::Ok ||
        m_electrum.state() != EndpointRow::State::Ok) {
        return TR("create-account-reason-backends");
    }
    return {};
}

void CreateAccount::updateActions() {
    QString reason = disabledReason();
    m_create->setEnabled(reason.isEmpty() && m_register_req_id == 0);
    m_actions->setReason(reason);
}

void CreateAccount::populateKeys() {
    m_keys_column->clear();
    m_keys_column->spacing(resolve(Spacing::S));
    m_key_rows.clear();
    m_keys.clear();

    auto *bus = Host::get()->keys();
    if (bus != nullptr) {
        m_keys = bus->keys(network());
    }

    for (const auto &entry : m_keys) {
        auto *row = new catalog::SelectRow;
        bool reachable = entry.descriptor_route != KeyRouteKind::Unknown;
        row->setIcon(reachable ? catalog::routeIcon(entry.descriptor_route) : icon::key);
        row->setTitle(entry.alias.isEmpty() ? entry.fingerprint : entry.alias);
        row->setMeta(entry.fingerprint);
        row->setSub(
            reachable
                ? TR("create-account-key-route").arg(catalog::routeLabel(entry.descriptor_route))
                : TR("create-account-key-no-route"));
        row->setEnabled(reachable);
        row->setProperty("fingerprint", entry.fingerprint);
        connect(row, &catalog::SelectRow::clicked, this, &CreateAccount::onKeyClicked,
                qontrol::UNIQUE);
        m_keys_column->push(row);
        // A row added to a shown column stays hidden until the event loop shows it,
        // so the step would be measured without it.
        row->show();
        m_key_rows.append(row);
    }

    if (m_keys.isEmpty()) {
        // The keys module answers for the network it holds, so other networks count
        // only the keys it has loaded.
        int elsewhere = 0;
        QStringList networks;
        for (auto other : NETWORKS) {
            if (other == network() || bus == nullptr) {
                continue;
            }
            auto count = static_cast<int>(bus->keys(other).size());
            if (count > 0) {
                elsewhere += count;
                networks.append(keys::networkLabel(other));
            }
        }
        catalog::EmptyState *empty = nullptr;
        if (elsewhere == 0) {
            empty = new catalog::EmptyState(icon::key, TR("create-account-no-key-title"),
                                            TR("create-account-no-key-sub"));
        } else {
            QString here = keys::networkLabel(network());
            QString sub = elsewhere == 1
                              ? TR("create-account-no-key-on-sub-one").arg(networks.first(), here)
                              : TR("create-account-no-key-on-sub-many")
                                    .arg(QString::number(elsewhere), networks.join(", "), here);
            empty = new catalog::EmptyState(icon::key,
                                            TR("create-account-no-key-on-title").arg(here), sub);
        }
        auto *openKeys = catalog::btnOpenKeysTab();
        connect(openKeys, &QPushButton::clicked, this, &CreateAccount::onOpenKeysTab,
                qontrol::UNIQUE);
        empty->setAction(openKeys);
        m_keys_column->push(empty);
        empty->show();
    }

    m_fetch_row->setVisible(!m_keys.isEmpty());
    refreshKeyRows();
}

void CreateAccount::refreshKeyRows() {
    for (auto *row : m_key_rows) {
        if (row->isEnabled()) {
            row->setSelected(row->property("fingerprint").toString() == m_key_fp);
        }
    }
    bool picked = !m_key_fp.isEmpty();
    bool fetched = !m_descriptor.isEmpty();
    m_get_descriptor->setVisible(!fetched);
    m_get_again->setVisible(fetched);
    m_get_descriptor->setEnabled(picked);
    m_get_again->setEnabled(picked);
    m_pick_key->setVisible(!picked);
}

void CreateAccount::onKeyClicked() {
    auto *row = qobject_cast<catalog::SelectRow *>(sender());
    if (row == nullptr) {
        return;
    }
    auto fingerprint = row->property("fingerprint").toString();
    if (fingerprint != m_key_fp) {
        m_descriptor.clear();
        m_descriptor_fingerprint.clear();
        m_descriptor_field->setVisible(false);
    }
    m_key_fp = fingerprint;
    m_fetch_error->setVisible(false);
    refreshKeyRows();
    updateActions();
    refit();
}

void CreateAccount::onNetworkChanged() {
    m_key_fp.clear();
    m_fetch_error->setVisible(false);
    populateKeys();
    applyNetworkDefaults();
    updateActions();
    refit();
}

void CreateAccount::onOpenKeysTab() {
    AppController::get()->showKeysTab();
    reject();
}

void CreateAccount::onFetchClicked() {
    if (m_key_fp.isEmpty() || Host::get()->keys() == nullptr) {
        return;
    }
    m_fetch_requested = true;
    done(FETCH_RESULT);
}

void CreateAccount::fetchDescriptor() {
    auto *bus = Host::get()->keys();
    if (!m_fetch_requested || bus == nullptr) {
        return;
    }
    m_fetch_requested = false;
    m_fetch_error->setVisible(false);
    m_descriptor_req_id = bus->newRequest();
    // Returns once the flow's window has closed, its answer delivered.
    bus->spDescriptor(m_descriptor_req_id, m_key_fp);
    refreshKeyRows();
    updateActions();
    refit();
}

void CreateAccount::onSpDescriptorReady(ReqId req_id, const QString &descriptor,
                                        const QString &fingerprint) {
    if (req_id != m_descriptor_req_id) {
        return;
    }
    m_descriptor_req_id = 0;
    m_descriptor = descriptor;
    m_descriptor_fingerprint = fingerprint;
    m_descriptor_network = network();
    m_descriptor_field->setValue(descriptor);
    m_descriptor_field->setVisible(true);
}

void CreateAccount::onKeysCancelled(ReqId req_id) {
    if (req_id == m_descriptor_req_id) {
        m_descriptor_req_id = 0;
    }
}

void CreateAccount::onKeysError(std::optional<ReqId> req_id, const QString &message) {
    if (!req_id.has_value()) {
        return;
    }
    if (req_id.value() == m_descriptor_req_id) {
        m_descriptor_req_id = 0;
        showFetchError(message);
        return;
    }
    if (req_id.value() == m_register_req_id) {
        m_register_req_id = 0;
        showFetchError(message);
        updateActions();
        refit();
    }
}

void CreateAccount::showFetchError(const QString &message) {
    m_fetch_error->setText(message);
    m_fetch_error->setVisible(true);
}

void CreateAccount::onCreate() {
    auto *bus = Host::get()->keys();
    if (!disabledReason().isEmpty() || bus == nullptr) {
        return;
    }
    m_fetch_error->setVisible(false);
    m_register_req_id = bus->newRequest();
    updateActions();
    bus->registerDescriptor(m_register_req_id, m_descriptor_fingerprint, m_descriptor);
}

void CreateAccount::onDescriptorRegistered(ReqId req_id) {
    if (req_id != m_register_req_id) {
        return;
    }
    m_register_req_id = 0;

    auto name = m_name_input->text().trimmed();
    auto blindbitUrl = m_blindbit.input()->text().trimmed();
    auto electrumUrl = m_electrum.input()->text().trimmed();
    auto config = new_config(rust::String(name.toStdString()), network(),
                             rust::String(m_descriptor.toStdString()),
                             rust::String(blindbitUrl.toStdString()),
                             rust::String(electrumUrl.toStdString()), 546, rust::String("sp"));
    config->to_file();

    AppController::get()->onAccountCreated(name);
    accept();
}

void CreateAccount::applyNetworkDefaults() {
    QString blindbit;
    QString electrum;
    bool autoTest = false;
    switch (network()) {
    case Network::Regtest: {
        auto defaults = AppController::get()->regtestDefaults();
        if (defaults.has_value()) {
            blindbit = defaults.value().blindbit_url;
            electrum = defaults.value().electrum_url;
        } else {
            blindbit = REGTEST_BLINDBIT_URL;
        }
        autoTest = true;
        break;
    }
    case Network::Bitcoin:
        blindbit = MAINNET_BLINDBIT_URL;
        electrum = MAINNET_ELECTRUM_URL;
        autoTest = true;
        break;
    default:
        break;
    }

    m_blindbit.input()->setText(blindbit);
    m_electrum.input()->setText(electrum);
    m_blindbit.setState(EndpointRow::State::Untested);
    m_electrum.setState(EndpointRow::State::Untested);
    if (autoTest) {
        // Test the defaults rather than assume them: only a passing test resolves the
        // scheme that gets saved.
        onTestBlindbit();
        onTestElectrum();
    }
}

void CreateAccount::onBlindbitEdited() {
    m_blindbit.setState(EndpointRow::State::Untested);
    updateActions();
    refit();
}

void CreateAccount::onElectrumEdited() {
    m_electrum.setState(EndpointRow::State::Untested);
    updateActions();
    refit();
}

void CreateAccount::onTestBlindbit() {
    auto url = m_blindbit.input()->text().trimmed();
    if (url.isEmpty()) {
        return;
    }
    m_blindbit.setState(EndpointRow::State::Testing);
    updateActions();
    auto *thread = QThread::create([this, url = url.toStdString()]() {
        auto info = ::get_backend_info(rust::String(url));
        emit backendInfoReady(info);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void CreateAccount::onBackendInfoReady(const BackendInfo &info) {
    // An edit or a network change since the test started voids its answer.
    if (m_blindbit.state() != EndpointRow::State::Testing) {
        return;
    }
    if (!info.is_ok) {
        auto rawError = QString::fromStdString(std::string(info.error));
        m_blindbit.setState(EndpointRow::State::Failed, mapBackendErrorSummary(rawError));
    } else if (info.network != network()) {
        m_blindbit.setState(EndpointRow::State::Failed, TR("create-account-backend-network")
                                                            .arg(keys::networkLabel(info.network),
                                                                 keys::networkLabel(network())));
    } else {
        m_blindbit.input()->setText(QString::fromStdString(std::string(info.url)));
        m_blindbit.setState(EndpointRow::State::Ok);
    }
    updateActions();
    refit();
}

void CreateAccount::onTestElectrum() {
    auto address = m_electrum.input()->text().trimmed();
    if (address.isEmpty()) {
        return;
    }
    m_electrum.setState(EndpointRow::State::Testing);
    updateActions();
    auto *thread = QThread::create([this, address = address.toStdString()]() {
        auto result = ::test_electrum(rust::String(address));
        emit electrumTestReady(result);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void CreateAccount::onElectrumTestReady(const ConnectionResult &result) {
    if (m_electrum.state() != EndpointRow::State::Testing) {
        return;
    }
    if (!result.is_ok) {
        auto rawError = QString::fromStdString(std::string(result.error));
        m_electrum.setState(EndpointRow::State::Failed, mapBackendErrorSummary(rawError));
    } else {
        m_electrum.input()->setText(QString::fromStdString(std::string(result.url)));
        m_electrum.setState(EndpointRow::State::Ok);
    }
    updateActions();
    refit();
}

void CreateAccount::refit() {
    showStep(m_step);
}

} // namespace modal
