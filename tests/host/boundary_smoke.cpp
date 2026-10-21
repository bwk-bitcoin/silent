#include "MockPlugin.h"
#include "PriceProbe.h"

#include "AppController.h"
#include "account/ElectrumReconnect.h"
#include "catalog/SelectRow.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "host/Host.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/musig/MusigStatusBar.h"
#include "modules/sp/SpPlugin.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <common.h>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#ifdef Q_OS_UNIX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Probe the real views without adding production-only test accessors.
#define private public
#define protected public
#include "modules/keys/modals/addkey/FileFlow.h"
#include "modules/keys/modals/addkey/KeyReview.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/SignModal.h"
#include "modules/musig/creator/CreatePolicy.h"
#include "modules/sp/AccountController.h"
#include "modules/sp/modals/CreateAccount.h"
#undef protected
#undef private

namespace smoke {
namespace {

class NumericProgressSpy final : public QObject {
    Q_OBJECT

public:
    uint32_t current = 0;
    uint32_t end = 0;
    uint32_t tip = 0;

public slots:

    void onProgress(uint32_t height, uint32_t last) {
        current = height;
        end = last;
    }

    void onWaiting(uint32_t height) {
        tip = height;
    }
};

class EndpointSpy final : public QObject {
    Q_OBJECT
public:
    QList<uint64_t> ids;
    QList<EndpointResult> results;
public slots:

    void onFinished(uint64_t req_id, [[maybe_unused]] Endpoint endpoint,
                    const EndpointResult &result) {
        ids.append(req_id);
        results.append(result);
    }
};

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("Rust boundary smoke: %s", message);
    }
}

template <typename Predicate> void waitFor(Predicate predicate, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

struct SoftwareKey {
    QString fingerprint;
    QString sp;
    QString policy;
};

#ifdef Q_OS_UNIX
class VersionPeer {
public:
    VersionPeer() {
        m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
        require(m_socket >= 0, "create local version socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(m_socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
                "bind local version socket");
        require(::listen(m_socket, 1) == 0, "listen for local version handshake");
        socklen_t size = sizeof(address);
        require(::getsockname(m_socket, reinterpret_cast<sockaddr *>(&address), &size) == 0,
                "read local version port");
        m_url = QStringLiteral("tcp://127.0.0.1:%1").arg(ntohs(address.sin_port));
        m_worker = std::thread([this] {
            pollfd descriptor{.fd = m_socket, .events = POLLIN, .revents = 0};
            require(::poll(&descriptor, 1, 10000) == 1, "endpoint Test reaches local peer");
            auto client = ::accept(m_socket, nullptr, nullptr);
            require(client >= 0, "accept version handshake");
            timeval timeout{.tv_sec = 5, .tv_usec = 0};
            require(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
                    "set version read timeout");
            QByteArray request;
            char character = 0;
            while (::read(client, &character, 1) == 1) {
                request.append(character);
                if (character == '\n') {
                    break;
                }
            }
            auto object = QJsonDocument::fromJson(request).object();
            require(object.value(QStringLiteral("method")).toString() ==
                        QStringLiteral("server.version"),
                    "actual version handshake request");
            QJsonObject response{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), object.value(QStringLiteral("id"))},
                {QStringLiteral("result"),
                 QJsonArray{QStringLiteral("status-probe"), QStringLiteral("1.4")}}};
            auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
            require(::write(client, bytes.data(), bytes.size()) == bytes.size(),
                    "write local version response");
            ::close(client);
        });
    }

    ~VersionPeer() {
        m_worker.join();
        ::close(m_socket);
    }

    [[nodiscard]] auto url() const -> QString {
        return m_url;
    }

private:
    int m_socket = -1;
    QString m_url;
    std::thread m_worker;
};
#endif

auto softwareKey() -> SoftwareKey {
    auto result = std::make_shared<SoftwareKey>();
    auto *worker = QThread::create([result] {
        auto sealed = ::hot_seal("abandon abandon abandon abandon abandon abandon abandon abandon "
                                 "abandon abandon abandon about",
                                 "boundary password", Network::Regtest);
        require(sealed->is_ok(), "software key seals");
        result->fingerprint = QString::fromStdString(std::string(sealed->fingerprint()));
        auto keys = ::new_keys_module(Network::Regtest);
        auto added = keys->commit_hot_key(*sealed, "Boundary software key");
        require(added.is_ok, "software key commits");
        auto material = keys->hot_material(sealed->fingerprint());
        auto sp = ::hot_sp_descriptor(*material, "boundary password");
        require(sp.is_ok, "software SP descriptor derives");
        result->sp = QString::fromStdString(std::string(sp.descriptor));
        auto xpub = ::hot_xpub(*material, "boundary password", "m/87'/1'/0'/0");
        require(xpub.is_ok, "software policy xpub derives");
        result->policy = QStringLiteral("tr([%1/87'/1'/0'/0]%2/<0;1>/*)")
                             .arg(QString::fromStdString(std::string(sealed->fingerprint())),
                                  QString::fromStdString(std::string(xpub.xpub)));
    });
    worker->start();
    worker->wait();
    delete worker;
    return *result;
}

void spCreation(const SoftwareKey &key, SignalSpy &launcher) {
    modal::CreateAccount view;
    view.show();
    waitFor(
        [&view] {
            return view.m_blindbit.state() != catalog::EndpointRow::State::Testing &&
                   view.m_electrum.state() != catalog::EndpointRow::State::Testing;
        },
        "SP endpoint fixture finishes");
    view.m_name_input->setText(QStringLiteral("boundary-sp"));
    view.m_electrum.input()->setText(QStringLiteral("tcp://127.0.0.1:1"));
    auto before = launcher.count();
    view.finishCreate();
    require(!view.isEnabled(), "SP creation disables editing while its worker owns the request");
    view.reject();
    waitFor([&view] { return view.m_creation_worker == nullptr; }, "SP failure returns");
    require(view.result() != QDialog::Accepted && launcher.count() == before,
            "failed SP creation neither accepts nor refreshes the launcher");
    require(view.isEnabled() && !view.m_fetch_error->isHidden(),
            "SP failure restores editing and presents an error");
    view.m_descriptor = key.sp;
    view.finishCreate();
    waitFor([&view] { return view.m_creation_worker == nullptr; }, "SP success returns");
    require(view.result() == QDialog::Accepted && launcher.count() == before + 1,
            "confirmed SP creation accepts and refreshes exactly once");
    require(::config_from_file("boundary-sp")->is_ok(),
            "SP acceptance follows a readable saved account");
}

void policyCreation(const SoftwareKey &key, SignalSpy &launcher) {
    musig::modal::CreatePolicy view;
    view.show();
    waitFor([&view] { return view.m_electrum.state() != catalog::EndpointRow::State::Testing; },
            "policy endpoint fixture finishes");
    view.m_name_input->setText(QStringLiteral("boundary-policy"));
    view.m_electrum.input()->setText(QStringLiteral("tcp://127.0.0.1:1"));
    auto before = launcher.count();
    view.install();
    require(!view.isEnabled(), "policy creation freezes editing during the request");
    view.reject();
    waitFor([&view] { return view.m_creation_worker == nullptr; }, "policy failure returns");
    require(view.result() != QDialog::Accepted && launcher.count() == before,
            "failed policy creation neither accepts nor refreshes the launcher");
    require(view.isEnabled() && !view.m_status_label->text().isEmpty(),
            "policy failure is presented");
    view.m_descriptor = key.policy;
    view.install();
    waitFor([&view] { return view.m_creation_worker == nullptr; }, "policy success returns");
    require(view.result() == QDialog::Accepted && launcher.count() == before + 1,
            "confirmed policy creation accepts and refreshes exactly once");
    require(::config_from_file("boundary-policy")->is_ok(),
            "policy acceptance follows a readable saved account");
}

void compiledPolicyPresentation(const SoftwareKey &key) {
    musig::modal::CreatePolicy view;
    waitFor([&view] { return view.m_electrum.state() != catalog::EndpointRow::State::Testing; },
            "review endpoint fixture finishes");
    view.m_compiled_review = ::review_policy(rust::String(key.policy.toStdString()));
    require(view.m_compiled_review.is_ok && !view.m_compiled_review.paths.empty(),
            "real policy lifts for review");
    musig::modal::SpendPath misleading;
    misleading.keys.append(QStringLiteral("ffffffff"));
    view.m_paths = {misleading};
    view.showReview();
    view.show();
    QApplication::processEvents();
    auto fingerprint =
        QString::fromStdString(std::string(view.m_compiled_review.paths[0].keys[0].fingerprint));
    bool compiled = false;
    for (auto *label : view.m_review_cards->findChildren<QLabel *>()) {
        compiled = compiled || label->text() == fingerprint;
        require(label->text() != QStringLiteral("ffffffff"),
                "review does not render the contradictory draft");
    }
    require(compiled, "review presents the fingerprint lifted from the compiled descriptor");
}

auto hasText(QWidget &widget, const QString &text) -> bool {
    return std::ranges::any_of(widget.findChildren<QLabel *>(),
                               [&text](QLabel *label) { return label->text() == text; });
}

auto signerRow(musig::SignModal &view, const QString &fingerprint) -> catalog::SelectRow * {
    for (auto *row : view.m_rows->findChildren<catalog::SelectRow *>()) {
        if (row->property("fingerprint").toString() == fingerprint) {
            return row;
        }
    }
    qFatal("Rust boundary smoke: signer row missing");
}

void signerPlanPresentation(const SoftwareKey &key) {
    MusigAccountController controller(QStringLiteral("missing-boundary-account"), nullptr);
    controller.m_spend_signers = {key.fingerprint};
    musig::SignModal view(&controller);
    view.m_plan = std::make_shared<SigningPlan>();
    view.m_plan->is_ok = true;
    view.m_plan_ready = true;
    SigningCandidate candidate{};
    candidate.fingerprint = rust::String(key.fingerprint.toStdString());
    candidate.route = KeyRouteKind::Hot;
    candidate.can_sign = true;
    candidate.unusable = KeyUnusableReason::None;
    candidate.signatures_done = true;
    view.m_plan->candidates.push_back(candidate);
    MusigRoundStatus status{};
    status.is_ok = true;
    status.round = MusigRound::NotMusig;
    view.render(status);
    require(hasText(*view.m_rows, TR("pill-signed")),
            "restored ordinary completion is displayed from the Rust plan");
    require(signerRow(view, key.fingerprint)->focusPolicy() == Qt::NoFocus,
            "a restored completed signer cannot be selected again");
    candidate.nonces_done = true;
    candidate.signatures_done = false;
    view.m_plan->candidates[0] = candidate;
    status.round = MusigRound::Nonces;
    MusigCosignerStatus cosigner{};
    cosigner.fingerprint = candidate.fingerprint;
    cosigner.expected = 1;
    status.cosigners.push_back(cosigner);
    view.m_partial = false;
    view.render(status);
    require(hasText(*view.m_rows, TR("pill-nonce")),
            "nonce completion follows the plan, not reconstructed round counts");
    candidate.can_sign = false;
    candidate.unusable = KeyUnusableReason::UnsupportedMusig;
    view.m_plan->candidates[0] = candidate;
    view.m_partial = true;
    view.render(status);
    require(view.m_lead->text() == TR("musig-sign-lead-partial"),
            "the displayed step remains separate from the protocol round");
    require(hasText(*view.m_rows, TR("pill-unsupported")), "typed capability refusal is presented");
    candidate.fingerprint = rust::String("ffffffff");
    candidate.route = KeyRouteKind::Usb;
    candidate.can_sign = true;
    candidate.unusable = KeyUnusableReason::None;
    candidate.nonces_done = false;
    view.m_plan->candidates[0] = candidate;
    status.cosigners[0].fingerprint = candidate.fingerprint;
    view.m_partial = false;
    view.render(status);
    require(signerRow(view, QStringLiteral("ffffffff"))->focusPolicy() != Qt::NoFocus,
            "Rust cached-nonce eligibility is not rejected by missing Qt capability metadata");
    view.reject();
}

void numericNotificationDelivery() {
    AccountController controller(static_cast<AccountWidget *>(nullptr));
    NumericProgressSpy spy;
    QObject::connect(&controller, &AccountController::scanProgress, &spy,
                     &NumericProgressSpy::onProgress, qontrol::UNIQUE);
    QObject::connect(&controller, &AccountController::waitingForBlocks, &spy,
                     &NumericProgressSpy::onWaiting, qontrol::UNIQUE);
    Notification notification{};
    notification.flag = NotificationFlag::ScanProgress;
    notification.current = 123;
    notification.end = 456;
    notification.payload = rust::String("not numeric text");
    controller.handleNotification(notification);
    require(spy.current == 123 && spy.end == 456,
            "Qt delivers numeric scanner progress without parsing payload text");
    notification.flag = NotificationFlag::WaitingForBlocks;
    notification.current = 789;
    controller.handleNotification(notification);
    require(spy.tip == 789, "Qt delivers the numeric waiting tip");
}

void portableKeyPresentation(keys::KeysModule &module, const SoftwareKey &key) {
    module.setNetwork(Network::Regtest);
    auto removed = module.removeKey(key.fingerprint, true);
    require(removed.is_ok, "remove isolated software key before portable revival");
    QTemporaryDir directory;
    require(directory.isValid(), "portable Qt fixture directory");
    auto path = directory.filePath(QStringLiteral("key.json"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "open invalid portable fixture");
    file.write("{}");
    file.close();
    keys::modal::AddKey dialog(&module);
    dialog.show();
    for (auto *row : dialog.m_mode_rows) {
        if (hasText(*row, TR("keys-add-mode-file"))) {
            row->clicked();
            break;
        }
    }
    auto *fileFlow = qobject_cast<keys::modal::FileFlow *>(dialog.m_flow);
    require(fileFlow != nullptr, "real Add Key routing creates the file flow");
    auto &flow = *fileFlow;
    flow.loadFile(path);
    waitFor([&flow] { return flow.m_worker == nullptr; }, "invalid portable preview returns");
    require(flow.m_view == keys::modal::FileFlow::View::Drop && !flow.m_file_error->isHidden(),
            "Rust portable parse failure is presented without leaving Drop");
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open valid portable fixture");
    auto json =
        QStringLiteral("{\"version\":1,\"network\":\"regtest\",\"fingerprint\":\"%1\",\"alias\":"
                       "\"Portable Qt import\",\"routes\":[\"sdcard\"],\"xpubs\":[]}")
            .arg(key.fingerprint);
    file.write(json.toUtf8());
    file.close();
    flow.loadFile(path);
    waitFor([&flow] { return flow.m_worker == nullptr; }, "portable preview returns");
    require(flow.m_view == keys::modal::FileFlow::View::Review && flow.m_preview->value()->is_ok(),
            "Rust-validated preview reaches Review");
    require(flow.m_review->alias() == QStringLiteral("Portable Qt import"),
            "review presents imported alias");
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "replace file after review");
    file.write("{}");
    file.close();
    SignalSpy added;
    SignalSpy back;
    QObject::connect(&flow, &keys::modal::AddKeyFlow::added, &added, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    QObject::connect(&flow, &keys::modal::AddKeyFlow::back, &back, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    flow.onNextClicked();
    flow.onBackClicked();
    require(back.count() == 0, "commit cannot be abandoned through Back");
    dialog.done(QDialog::Rejected);
    require(dialog.isVisible() && dialog.m_flow == &flow,
            "window dismissal cannot abandon a non-cancellable commit");
    waitFor([&added] { return added.count() == 1; }, "portable commit returns");
    require(added.count() == 1, "one confirmed portable commit");
    require(dialog.result() == QDialog::Accepted && dialog.m_flow == nullptr,
            "successful import closes its owning modal through the signal chain");
    require(module.hasKey(key.fingerprint),
            "pure committed-snapshot adoption refreshes the existing Qt key cache");
    require(::new_keys_module(Network::Regtest)
                    ->key(rust::String(key.fingerprint.toStdString()))
                    .key.alias == rust::String("Portable Qt import"),
            "the confirmed alias is persisted despite replacement of the file after preview");
}

void retryPresentation() {
    account::ElectrumReconnect reconnect;
    SignalSpy requests;
    SignalSpy gaveUp;
    QObject::connect(&reconnect, &account::ElectrumReconnect::reconnect, &requests,
                     &SignalSpy::onSignal, qontrol::UNIQUE);
    QObject::connect(&reconnect, &account::ElectrumReconnect::gaveUp, &gaveUp, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    reconnect.connected();
    reconnect.lost();
    reconnect.onRetry();
    reconnect.lost();
    reconnect.onRetry();
    reconnect.lost();
    reconnect.onRetry();
    reconnect.lost();
    require(requests.count() == 3 && gaveUp.count() == 1,
            "Qt presents the Rust retry budget and exhaustion exactly once");
    reconnect.onRetry();
    require(requests.count() == 3, "exhausted retry cannot dispatch another request");
    reconnect.connected();
    reconnect.stopped();
    reconnect.lost();
    reconnect.onRetry();
    require(requests.count() == 3, "intentional disconnect suppresses retries");
}

void endpointWorkerPresentation() {
    auto config = ::config_from_file("boundary-sp");
    require(config->is_ok(), "read isolated endpoint fixture");
    config->set_electrum_url(rust::String());
    require(config->save().empty(), "start the endpoint worker fixture offline");
    AccountController controller(QStringLiteral("boundary-sp"), nullptr);
    require(controller.getAccount().has_value(), "real SP endpoint fixture opens");
    EndpointSpy replies;
    SignalSpy stopped;
    QObject::connect(&controller, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(&controller, &AccountController::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    controller.setEndpointConnected(41001, Endpoint::Electrum, false);
    controller.setEndpointConnected(41002, Endpoint::Electrum, false);
    require(replies.ids == QList<uint64_t>{41002} && !replies.results[0].is_ok,
            "a busy endpoint command is refused without replacing the active request");
    waitFor([&replies] { return replies.ids.size() == 2; },
            "Rust endpoint command and Qt receiver handoff finish");
    require(replies.ids[1] == 41001 && replies.results[1].is_ok &&
                !replies.results[1].connection_pending,
            "the original request completes offline only after worker/receiver drain");
    require(controller.getAccount().has_value() && controller.getAccount().value()->is_ok(),
            "Qt regains the Rust-owned account after completion");
    controller.stop();
    waitFor([&stopped] { return stopped.count() == 1; },
            "endpoint fixture shutdown drains its new receiver");
}

#ifdef Q_OS_UNIX
void musigEndpointStatusPresentation() {
    auto config = ::config_from_file("boundary-policy");
    require(config->is_ok(), "read isolated MuSig status fixture");
    config->set_electrum_url(rust::String());
    require(config->save().empty(), "start MuSig status fixture offline");
    MusigAccountController controller(QStringLiteral("boundary-policy"), nullptr);
    require(controller.getAccount().has_value() && controller.getAccount().value()->is_ok(),
            "real MuSig status fixture opens");
    MusigStatusBar bar(&controller);
    EndpointSpy replies;
    SignalSpy stopped;
    QObject::connect(&controller, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(&controller, &MusigAccountController::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    {
        VersionPeer peer;
        controller.testAndSaveEndpoint(42001, Endpoint::Electrum, peer.url(), true);
        waitFor([&replies] { return replies.ids.size() == 1; },
                "real MuSig Test/save completes offline");
        require(replies.results[0].is_ok && replies.results[0].saved &&
                    !replies.results[0].connection_pending,
                "Test/save is complete without a pending connection");
        require(controller.electrumState() == MusigAccountController::ElectrumState::Disconnected,
                "backend reports intentional offline state after Test/save");
        require(hasText(bar, TR("status-electrum-disconnected")) &&
                    !hasText(bar, TR("status-connecting")),
                "URL reload cannot turn an intentional offline Test/save into Connecting");
        bar.onReloadUrl();
        require(hasText(bar, TR("status-electrum-disconnected")),
                "repeated URL refresh preserves offline presentation");
    }
    controller.setEndpointConnected(42002, Endpoint::Electrum, true);
    require(controller.electrumState() == MusigAccountController::ElectrumState::Connecting &&
                hasText(bar, TR("status-connecting")),
            "an accepted real start presents Connecting before completion");
    require(!controller.isEndpointRunning(Endpoint::Electrum),
            "pending start is not a confirmed live connection");
    waitFor([&replies] { return replies.ids.size() == 2; },
            "closed local peer reports the real start failure");
    require(!replies.results[1].is_ok &&
                controller.electrumState() == MusigAccountController::ElectrumState::Disconnected,
            "failed start returns to offline backend state");
    require(hasText(bar, TR("status-electrum-disconnected")), "failed start clears Connecting");
    controller.stop();
    waitFor([&stopped] { return stopped.count() == 1; },
            "MuSig status fixture drains its receiver");
}
#endif

} // namespace

void boundarySmoke() {
    ::app_set_last_endpoint(BackendServer::Blindbit, Network::Regtest, "http://127.0.0.1:1");
    ::app_set_last_endpoint(BackendServer::Electrum, Network::Regtest, "invalid-endpoint");
    AppController::get()->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    AppController::get()->registerBuiltinPlugin(std::make_unique<MusigPlugin>());
    auto keysModule = std::make_unique<keys::KeysModule>();
    auto *keys = keysModule.get();
    AppController::get()->registerBuiltinPlugin(std::move(keysModule));
    SignalSpy launcher;
    QObject::connect(Host::get()->events(), &HostEvents::launcherRefreshRequested, &launcher,
                     &SignalSpy::onSignal, qontrol::UNIQUE);
    auto key = softwareKey();
    spCreation(key, launcher);
    policyCreation(key, launcher);
    compiledPolicyPresentation(key);
    signerPlanPresentation(key);
    numericNotificationDelivery();
    endpointWorkerPresentation();
#ifdef Q_OS_UNIX
    musigEndpointStatusPresentation();
#endif
    portableKeyPresentation(*keys, key);
    retryPresentation();
}

} // namespace smoke

#include "boundary_smoke.moc"
