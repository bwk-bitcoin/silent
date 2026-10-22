#include "MockPlugin.h"
#include "PriceProbe.h"
#include "account_navigation_smoke.h"

#include "AppController.h"
#include "MainWindow.h"
#include "account/ElectrumReconnect.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/StatusBarItem.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/Toggle.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/account.h"
#include "modules/keys/KeysModule.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/musig/MusigStatusBar.h"
#include "modules/sp/AccountWidget.h"
#include "modules/sp/SpPlugin.h"
#include "views/MenuTab.h"
#include "views/modals/ConfirmDelete.h"
#include "views/utils.h"

#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <common.h>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
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
#include "account/views/Settings.h"
#include "modules/keys/modals/addkey/FileFlow.h"
#include "modules/keys/modals/addkey/KeyReview.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/SignModal.h"
#include "modules/musig/creator/CreatePolicy.h"
#include "modules/sp/AccountController.h"
#include "modules/sp/StatusBar.h"
#include "modules/sp/modals/CreateAccount.h"
#undef protected
#undef private

namespace smoke {
namespace {

void require(bool condition, const char *message);

void captureDeletionWindow(MainWindow &window, const QString &path) {
    auto *screen = window.screen();
    require(screen != nullptr, "the private display has a screen");
    QCursor::setPos(screen->geometry().topLeft());
    auto shot = screen->grabWindow(window.winId());
    require(!shot.isNull() && shot.save(path),
            "whole visible window including its native modal is captured");
}

class DialogExposure {
public:
    auto ready(QDialog *dialog) -> bool {
        if (m_dialog != dialog) {
            m_dialog = dialog;
            m_visible.start();
            return false;
        }
        return m_visible.hasExpired(100);
    }

private:
    QPointer<QDialog> m_dialog;
    QElapsedTimer m_visible;
};

class DeleteConfirmationDriver final : public QObject {
    Q_OBJECT

public:
    explicit DeleteConfirmationDriver(MainWindow &window) : m_window(window) {
        connect(&m_timer, &QTimer::timeout, this, &DeleteConfirmationDriver::onTick,
                qontrol::UNIQUE);
        m_timer.start(5);
    }

    void confirmDelete() {
        m_confirm = true;
    }

    [[nodiscard]] auto seen() const -> int {
        return m_seen;
    }

    [[nodiscard]] auto text() const -> const QString & {
        return m_text;
    }

public slots:

    void onTick() {
        auto *dialog = qobject_cast<modal::ConfirmDelete *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        auto shots = qEnvironmentVariable("SILENT_DELETION_SHOTS");
        if (!shots.isEmpty() && !m_exposure.ready(dialog)) {
            return;
        }
        ++m_seen;
        for (auto *label : dialog->findChildren<QLabel *>()) {
            m_text += label->text();
        }
        if (!shots.isEmpty()) {
            captureDeletionWindow(m_window,
                                  shots + (m_confirm ? QStringLiteral("/confirmation-delete.png")
                                                     : QStringLiteral("/confirmation-cancel.png")));
        }
        if (!m_confirm) {
            dialog->reject();
            return;
        }
        for (auto *button : dialog->findChildren<QPushButton *>()) {
            if (button->text() == TR("common-delete")) {
                button->click();
                return;
            }
        }
        qFatal("deletion confirmation has no Delete action");
    }

private:
    MainWindow &m_window;
    bool m_confirm = false;
    int m_seen = 0;
    QString m_text;
    DialogExposure m_exposure;
    QTimer m_timer;
};

class DeletionSpy final : public QObject {
    Q_OBJECT

public:
    QStringList ids;
    QStringList errors;
    QMap<ReqId, QString> account_errors;

public slots:

    void onFinished(IModule *module, const QString &id, const QString &error) {
        Q_UNUSED(module);
        ids.append(id);
        errors.append(error);
    }

    void onAccountError(std::optional<ReqId> req_id, const QString &error) {
        if (req_id.has_value()) {
            account_errors.insert(*req_id, error);
        }
    }
};

class DeletionErrorObserver final : public QObject {
    Q_OBJECT

public:
    explicit DeletionErrorObserver(MainWindow &window) : m_window(window) {
        connect(&m_timer, &QTimer::timeout, this, &DeletionErrorObserver::onTick, qontrol::UNIQUE);
        m_timer.start(5);
    }

    [[nodiscard]] auto messages() const -> const QStringList & {
        return m_messages;
    }

public slots:

    void onTick() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        auto shots = qEnvironmentVariable("SILENT_DELETION_SHOTS");
        if (!shots.isEmpty() && !m_exposure.ready(dialog)) {
            return;
        }
        QStringList labels;
        for (auto *label : dialog->findChildren<QLabel *>()) {
            labels.append(label->text());
        }
        m_messages.append(labels.join(QStringLiteral(" ")));
        if (!shots.isEmpty()) {
            captureDeletionWindow(
                m_window, shots + QStringLiteral("/delete-error-%1.png").arg(m_messages.size()));
        }
        dialog->accept();
    }

private:
    MainWindow &m_window;
    QStringList m_messages;
    DialogExposure m_exposure;
    QTimer m_timer;
};

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
    QList<uint64_t> info_ids;
    QList<BackendInfo> infos;
    QList<NotificationFlag> notifications;
    uint32_t waiting_height = 0;
public slots:

    void onFinished(uint64_t req_id, [[maybe_unused]] Endpoint endpoint,
                    const EndpointResult &result) {
        ids.append(req_id);
        results.append(result);
    }

    void onInfo(uint64_t req_id, [[maybe_unused]] Endpoint endpoint, const BackendInfo &info) {
        info_ids.append(req_id);
        infos.append(info);
    }

    void onNotification([[maybe_unused]] uint64_t generation, const Notification &notification) {
        notifications.append(notification.flag);
        if (notification.flag == NotificationFlag::WaitingForBlocks) {
            waiting_height = notification.current;
        }
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

void editEndpoint(catalog::Input *input, const QString &text) {
    input->selectAll();
    QKeyEvent event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, text);
    QApplication::sendEvent(input, &event);
}

struct SoftwareKey {
    QString fingerprint;
    QString sp;
    QString policy;
};

#ifdef Q_OS_UNIX
class EndpointPeer {
public:
    enum class Protocol { Electrum, Blindbit, CaughtUpBlindbit };

    explicit EndpointPeer(unsigned requests = 1, bool reject_last = false, bool hold_last = false,
                          Protocol protocol = Protocol::Electrum) {
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
        m_url = (protocol != Protocol::Electrum ? QStringLiteral("http://127.0.0.1:%1")
                                                : QStringLiteral("tcp://127.0.0.1:%1"))
                    .arg(ntohs(address.sin_port));
        m_worker = std::thread([this, requests, reject_last, hold_last, protocol] {
            serve(requests, reject_last, hold_last, protocol);
        });
    }

    ~EndpointPeer() {
        m_stopping = true;
        m_release.release();
        close();
    }

    [[nodiscard]] auto url() const -> QString {
        return m_url;
    }

    [[nodiscard]] auto waiting() const -> bool {
        return m_waiting;
    }

    void release() {
        m_release.release();
    }

    void close() {
        if (m_worker.joinable()) {
            m_worker.join();
        }
        if (m_socket >= 0) {
            ::close(m_socket);
            m_socket = -1;
        }
    }

protected:
    void serve(unsigned requests, bool reject_last, bool hold_last, Protocol protocol) {
        bool blindbit = protocol != Protocol::Electrum;
        bool caughtUp = protocol == Protocol::CaughtUpBlindbit;
        bool heldHeight = false;
        for (unsigned requestIndex = 0; !m_stopping && (requests == 0 || requestIndex < requests);
             ++requestIndex) {
            pollfd descriptor{.fd = m_socket, .events = POLLIN, .revents = 0};
            auto ready = ::poll(&descriptor, 1, requests == 0 ? 100 : 10000);
            if (requests == 0 && ready == 0) {
                continue;
            }
            require(ready == 1, "endpoint Test reaches local peer");
            auto client = ::accept(m_socket, nullptr, nullptr);
            require(client >= 0, "accept endpoint handshake");
            timeval timeout{.tv_sec = 5, .tv_usec = 0};
            require(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
                    "set endpoint read timeout");
            QByteArray request;
            char character = 0;
            while (::read(client, &character, 1) == 1) {
                request.append(character);
                if (blindbit ? request.endsWith("\r\n\r\n") : character == '\n') {
                    break;
                }
            }
            bool last = requestIndex + 1 == requests;
            bool heightRequest = request.startsWith("GET /block-height ");
            if (hold_last && (last || (caughtUp && heightRequest && !heldHeight))) {
                m_waiting = true;
                heldHeight = true;
                m_release.acquire();
            }
            if (blindbit) {
                replyBlindbit(client, request, caughtUp);
            } else {
                replyElectrum(client, request, last && reject_last);
            }
            ::close(client);
            if (last && reject_last) {
                ::close(m_socket);
                m_socket = -1;
            }
        }
    }

    static void replyBlindbit(int client, const QByteArray &request, bool caught_up) {
        require(request.startsWith("GET /info ") ||
                    (caught_up && request.startsWith("GET /block-height ")),
                "actual BlindBit info or caught-up height request, never a block scan");
        QByteArray body =
            R"({"network":"regtest","height":123,"tweaks_only":true,"tweaks_full_basic":false,"tweaks_full_with_dust_filter":true,"tweaks_cut_through_with_dust_filter":false})";
        if (caught_up && request.startsWith("GET /block-height ")) {
            body = R"({"block_height":0})";
        } else if (caught_up) {
            body.replace("\"height\":123", "\"height\":0");
        }
        auto response =
            QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ") +
            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        require(::write(client, response.data(), response.size()) == response.size(),
                "write real BlindBit info response");
    }

    static void replyElectrum(int client, const QByteArray &request, bool reject) {
        auto document = QJsonDocument::fromJson(request);
        auto object = document.isArray() ? document.array().first().toObject() : document.object();
        auto method = object.value(QStringLiteral("method")).toString();
        require(method == QStringLiteral("server.version") ||
                    method == QStringLiteral("blockchain.headers.subscribe"),
                "actual Electrum handshake request");
        QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                             {QStringLiteral("id"), object.value(QStringLiteral("id"))},
                             {QStringLiteral("result"),
                              QJsonArray{QStringLiteral("status-probe"), QStringLiteral("1.4")}}};
        if (reject) {
            response.remove(QStringLiteral("result"));
            response.insert(QStringLiteral("error"),
                            QJsonObject{{QStringLiteral("code"), -1},
                                        {QStringLiteral("message"),
                                         QStringLiteral("endpoint handshake refused")}});
        }
        auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
        require(::write(client, bytes.data(), bytes.size()) == bytes.size(),
                "write local version response");
    }

private:
    int m_socket = -1;
    QString m_url;
    std::thread m_worker;
    std::atomic<bool> m_waiting = false;
    std::atomic<bool> m_stopping = false;
    std::binary_semaphore m_release{0};
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
    require(view.m_blindbit.state() == catalog::EndpointRow::State::Failed &&
                view.m_blindbit.input()->text() == QStringLiteral("http://127.0.0.1:1") &&
                !view.m_blindbit.note()->isHidden() &&
                view.m_electrum.state() == catalog::EndpointRow::State::Failed &&
                view.m_electrum.input()->text() == QStringLiteral("invalid-endpoint") &&
                !view.m_electrum.note()->isHidden(),
            "failed SP endpoint tests share the Failed state and keep their URLs");
    editEndpoint(view.m_electrum.input(), QStringLiteral("tcp://127.0.0.1:1"));
    require(view.m_electrum.state() == catalog::EndpointRow::State::Untested &&
                view.m_electrum.note()->isHidden() && view.m_electrum.testButton()->isEnabled(),
            "an edit returns the failed SP endpoint to untested");
    view.m_electrum.input()->selectAll();
    QKeyEvent erase(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(view.m_electrum.input(), &erase);
    require(view.m_electrum.input()->text().isEmpty() &&
                view.m_electrum.state() == catalog::EndpointRow::State::Untested &&
                !view.m_electrum.testButton()->isEnabled(),
            "an empty SP endpoint is untested and cannot be tested");
#ifdef Q_OS_UNIX
    {
        EndpointPeer peer(1, false, true);
        editEndpoint(view.m_electrum.input(), peer.url());
        view.m_electrum.testButton()->click();
        require(view.m_electrum.state() == catalog::EndpointRow::State::Testing &&
                    view.m_electrum.testButton()->isHidden() && view.m_electrum.note()->isHidden(),
                "a running SP endpoint test presents Testing");
        waitFor([&peer] { return peer.waiting(); }, "SP endpoint test reaches real peer");
        peer.release();
        waitFor([&view] { return view.m_electrum.state() != catalog::EndpointRow::State::Testing; },
                "SP endpoint test completes");
        require(view.m_electrum.state() == catalog::EndpointRow::State::Ok &&
                    view.m_electrum.input()->text().endsWith(peer.url().mid(6)) &&
                    view.m_electrum.note()->isHidden() && !view.m_electrum.testButton()->isHidden(),
                "a passing SP endpoint test presents Ok and keeps the tested URL");
    }
#endif
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
    MusigAccountController controller(QStringLiteral("boundary-policy"), nullptr);
    waitFor([&controller] { return !controller.accountLoading(); }, "signer account load finishes");
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
    SignalSpy stopped;
    QObject::connect(&controller, &MusigAccountController::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    controller.stop();
    waitFor([&stopped] { return stopped.count() == 1; }, "signer fixture workers drain");
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
    waitFor([&controller] { return !controller.accountLoading(); }, "SP offline opening finishes");
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

struct EndpointRequest {
    uint64_t id = 0;
    Endpoint endpoint = Endpoint::Electrum;
    QString url;
    bool verify = true;
};

// The real controller, with Test/save completed by the test instead of a live server.
template <typename Controller> class ControlledEndpointTest final : public Controller {
public:
    explicit ControlledEndpointTest(const QString &account_name)
        : Controller(account_name, nullptr),
          name(account_name) {
    }

    void testAndSaveEndpoint(uint64_t req_id, Endpoint endpoint, const QString &url,
                             bool verify_certificate) override {
        this->beginEndpointOperation(req_id, endpoint, EndpointOperation::Testing);
        request = EndpointRequest{
            .id = req_id, .endpoint = endpoint, .url = url, .verify = verify_certificate};
    }

    [[nodiscard]] auto pending(uint64_t req_id, const QString &url, bool verify) const -> bool {
        return request.has_value() && request->id == req_id &&
               request->endpoint == Endpoint::Electrum && request->url == url &&
               request->verify == verify && this->endpointOperationsBusy();
    }

    // Persists the requested tuple and finishes the operation as a passing Test.
    void completeTest() {
        require(request.has_value(), "a controlled Test is pending");
        auto tested = *request;
        request.reset();
        auto config = ::config_from_file(rust::String(name.toStdString()));
        config->set_electrum_url(rust::String(tested.url.toStdString()));
        config->set_verify_certificate(tested.verify);
        require(config->save().empty(), "controlled Test persists the tested endpoint");
        rust::String url(tested.url.toStdString());
        this->setSavedEndpoint(tested.endpoint, url, tested.verify);
        EndpointResult result{};
        result.kind = BackendErrorKind::None;
        result.is_ok = true;
        result.saved = true;
        result.url = url;
        result.verify_certificate = tested.verify;
        this->finishEndpointOperation(result);
    }

    QString name;
    std::optional<EndpointRequest> request;
};

// `tester` saves `verify` for the SSL server `observer` holds as tested.
template <typename Controller>
void changeSavedVerify(ControlledEndpointTest<Controller> &controller,
                       account::view::Settings::ServerRow *tester,
                       account::view::Settings::ServerRow *observer, const QString &url,
                       bool verify) {
    require(observer->field->state() == catalog::EndpointRow::State::Ok &&
                tester->field->input()->text() == url && tester->ssl->isChecked() &&
                tester->verify_wanted != verify,
            "both views hold the saved SSL server before the certificate change");
    tester->verify->click();
    require(tester->verify_wanted == verify && tester->field->testButton()->isEnabled() &&
                tester->field->state() == catalog::EndpointRow::State::Untested,
            "the certificate choice is a new draft to test");
    auto revision = observer->draft_revision;
    tester->field->testButton()->click();
    auto requestId = tester->action_req;
    require(requestId != 0 && controller.pending(requestId, url, verify) &&
                tester->field->state() == catalog::EndpointRow::State::Testing &&
                controller.verifiesCertificate() != verify,
            "the Test requests the SSL URL with the new certificate choice");
    require(observer->field->state() == catalog::EndpointRow::State::Ok &&
                observer->draft_revision == revision && observer->saved_verify != verify,
            "a pending Test leaves the observer's saved tuple and result alone");
    controller.completeTest();
    require(tester->action_req == 0 && tester->field->state() == catalog::EndpointRow::State::Ok &&
                tester->saved_url == url && tester->saved_verify == verify,
            "the tester presents Ok for the saved tuple");
    require(controller.endpointUrl(Endpoint::Electrum) == url &&
                controller.verifiesCertificate() == verify &&
                ::config_from_file(rust::String(controller.name.toStdString()))
                        ->get_verify_certificate() == verify,
            "the backend and the account config hold the new certificate choice");
    require(observer->field->state() == catalog::EndpointRow::State::Untested &&
                observer->draft_revision == revision + 1 && observer->saved_url == url &&
                observer->saved_verify == verify && observer->field->input()->text() == url &&
                observer->ssl->isChecked() && observer->verify_wanted == verify &&
                observer->verify->isChecked() == verify,
            "the observer shows the saved certificate choice and its result is invalidated");
}

template <typename Controller> void savedEndpointObservers(const QString &name) {
    ControlledEndpointTest<Controller> controller(name);
    waitFor([&controller] { return !controller.accountLoading(); },
            "controlled endpoint fixture opens offline");
    AccountBackend *backend = &controller;
    account::view::Settings first(backend);
    account::view::Settings second(backend);
    auto *row = first.row(Endpoint::Electrum);
    auto *other = second.row(Endpoint::Electrum);
    require(row != nullptr && other != nullptr && !first.isVisible() && !second.isVisible() &&
                backend->endpointUrl(Endpoint::Electrum).isEmpty(),
            "two hidden Settings views observe one offline backend");
    EndpointSpy replies;
    SignalSpy stopped;
    QObject::connect(backend, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(&controller, &Controller::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);

    auto ssl = QStringLiteral("ssl://localhost:50002");
    editEndpoint(row->field->input(), ssl);
    require(row->ssl->isChecked() && row->verify_wanted,
            "an SSL draft checks the certificate by default");
    row->field->testButton()->click();
    auto firstId = row->action_req;
    require(firstId != 0 && controller.pending(firstId, ssl, true) &&
                backend->endpointUrl(Endpoint::Electrum).isEmpty(),
            "the first view's SSL Test is pending without saving");
    controller.completeTest();
    require(replies.ids.last() == firstId &&
                row->field->state() == catalog::EndpointRow::State::Ok &&
                other->field->state() == catalog::EndpointRow::State::Untested &&
                other->field->input()->text() == ssl && other->verify->isChecked() &&
                backend->verifiesCertificate(),
            "the first view saves the SSL server with its certificate checked");

    changeSavedVerify(controller, other, row, ssl, false);
    changeSavedVerify(controller, row, other, ssl, true);

    auto rowRevision = row->draft_revision;
    auto otherRevision = other->draft_revision;
    emit backend->endpointStateChanged(Endpoint::Electrum);
    emit backend->endpointStateChanged(Endpoint::Electrum);
    require(row->draft_revision == rowRevision && other->draft_revision == otherRevision &&
                row->field->state() == catalog::EndpointRow::State::Ok &&
                other->field->state() == catalog::EndpointRow::State::Untested &&
                row->field->input()->text() == ssl && other->field->input()->text() == ssl,
            "notifications of an unchanged saved tuple invalidate no view");

    auto plain = QStringLiteral("tcp://localhost:50001");
    rowRevision = row->draft_revision;
    editEndpoint(other->field->input(), plain);
    require(!other->ssl->isChecked() && other->field->input()->text() == plain,
            "a plaintext draft turns SSL off");
    other->field->testButton()->click();
    auto plainId = other->action_req;
    require(plainId != 0 && controller.pending(plainId, plain, true),
            "a plaintext Test asks for the certificate check it cannot use");
    controller.completeTest();
    require(other->field->state() == catalog::EndpointRow::State::Ok &&
                row->field->input()->text() == plain && !row->ssl->isChecked() &&
                row->field->state() == catalog::EndpointRow::State::Untested &&
                row->draft_revision == rowRevision + 1 && row->saved_verify,
            "a URL-only saved change invalidates the observer's earlier success");
    rowRevision = row->draft_revision;
    otherRevision = other->draft_revision;
    uint64_t requestId = 65001;
    for (bool verify : {false, true}) {
        backend->testAndSaveEndpoint(requestId, Endpoint::Electrum, plain, verify);
        require(controller.pending(requestId, plain, verify),
                "a backend Test of the raw choice is pending");
        controller.completeTest();
        require(replies.ids.last() == requestId && backend->verifiesCertificate() == verify &&
                    backend->endpointUrl(Endpoint::Electrum) == plain,
                "the backend saves the plaintext server with its raw certificate choice");
        require(row->saved_verify && other->saved_verify && row->draft_revision == rowRevision &&
                    other->draft_revision == otherRevision &&
                    other->field->state() == catalog::EndpointRow::State::Ok &&
                    row->field->state() == catalog::EndpointRow::State::Untested &&
                    row->field->input()->text() == plain && other->field->input()->text() == plain,
                "a raw certificate change on a plaintext server invalidates no view");
        ++requestId;
    }

    controller.stop();
    waitFor([&stopped] { return stopped.count() == 1; }, "controlled endpoint fixture drains");
    auto config = ::config_from_file(rust::String(name.toStdString()));
    config->set_electrum_url(rust::String());
    config->set_verify_certificate(true);
    require(config->save().empty(), "controlled endpoint fixture is offline again");
}

#ifdef Q_OS_UNIX
void musigEndpointStatusPresentation() {
    auto config = ::config_from_file("boundary-policy");
    require(config->is_ok(), "read isolated MuSig status fixture");
    config->set_electrum_url(rust::String());
    require(config->save().empty(), "start MuSig status fixture offline");
    MusigAccountController controller(QStringLiteral("boundary-policy"), nullptr);
    waitFor([&controller] { return !controller.accountLoading(); },
            "MuSig offline opening finishes");
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
        EndpointPeer peer;
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
    require(hasText(bar, mapBackendErrorSummary(replies.results[1].kind)) &&
                bar.findChild<catalog::StatusBarItem *>()->toolTip() ==
                    QString::fromUtf8(replies.results[1].error.data(),
                                      replies.results[1].error.size()) &&
                !hasText(bar, TR("status-connecting")),
            "failed start displays its actual error rather than Connecting");
    controller.stop();
    waitFor([&stopped] { return stopped.count() == 1; },
            "MuSig status fixture drains its receiver");
}

template <typename Controller, typename Bar> void endpointInteractions(const QString &name) {
    Controller controller(name, nullptr);
    waitFor([&controller] { return !controller.accountLoading(); },
            "offline opening precedes views");
    AccountBackend *backend = &controller;
    account::view::Settings settings(backend);
    account::view::Settings observer(backend);
    Bar bar(&controller);
    auto *row = settings.row(Endpoint::Electrum);
    auto *other = observer.row(Endpoint::Electrum);
    auto *toggle = bar.template findChildren<catalog::StatusBarItem *>()
                       .last()
                       ->template findChild<catalog::Toggle *>();
    require(row != nullptr && other != nullptr && toggle != nullptr,
            "real Settings and status controls exist");
    EndpointSpy replies;
    SignalSpy stopped;
    SignalSpy connected;
    QObject::connect(&controller, &Controller::electrumConnected, &connected, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    QObject::connect(backend, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(&controller, &Controller::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    settings.show();
    observer.show();
    auto oldGeneration = controller.m_notif_generation;

    EndpointPeer peer(3, true, true);
    editEndpoint(row->field->input(), peer.url());
    row->field->testButton()->click();
    auto testId = row->action_req;
    require(testId != 0 && backend->endpointOperationsBusy() && !toggle->isEnabled() &&
                !other->connect->isEnabled(),
            "Settings Test publishes busy to every real initiator");
    waitFor([&replies] { return replies.ids.size() == 1; }, "real Settings Test/save finishes");
    require(replies.ids[0] == testId && replies.results[0].is_ok && replies.results[0].saved &&
                !replies.results[0].connection_pending &&
                !backend->isEndpointRunning(Endpoint::Electrum),
            "Test/save completes offline, not as a connected dispatch");
    require(row->saved_url == peer.url().mid(6) &&
                other->field->input()->text() == row->saved_url &&
                backend->endpointUrl(Endpoint::Electrum) == row->saved_url && toggle->isEnabled(),
            "saved URL reaches both Settings views and the status control");
    require(row->field->state() == catalog::EndpointRow::State::Ok &&
                row->field->note()->isHidden() &&
                other->field->state() == catalog::EndpointRow::State::Untested,
            "the tested view presents Ok, the observer shows the saved URL untested");
    require(!row->field->testButton()->isEnabled() && row->connect->isEnabled(),
            "the saved tuple offers Connect without another destructive Test");
    row->ssl->click();
    require(row->field->state() == catalog::EndpointRow::State::Untested &&
                !row->connect->isEnabled() && row->field->testButton()->isEnabled(),
            "changing SSL invalidates a successful test and requires testing the new tuple");
    row->ssl->click();
    editEndpoint(row->field->input(), QStringLiteral(" "));
    require(row->field->state() == catalog::EndpointRow::State::Untested &&
                !row->field->testButton()->isEnabled() && row->field->note()->isHidden(),
            "an empty Settings draft is untested and cannot be tested");
    editEndpoint(row->field->input(), row->saved_url);

    // A second successful Test leaves the same saved URL available to both initiators.
    backend->testAndSaveEndpoint(64001, Endpoint::Electrum, peer.url(), true);
    waitFor([&replies] { return replies.ids.size() == 2; }, "second public Test/save completes");
    require(replies.results[1].is_ok, "second successful Test remains offline");
    auto testState = row->field->state();
    row->connect->click();
    auto connectId = row->action_req;
    waitFor([&peer] { return peer.waiting(); }, "Connect reaches the held failing handshake");
    require(connectId != 0 && replies.ids.size() == 2 && backend->endpointOperationsBusy() &&
                !backend->isEndpointRunning(Endpoint::Electrum) && !toggle->isEnabled() &&
                !other->connect->isEnabled(),
            "accepted Connect is pending, not connected or completed");
    backend->setEndpointConnected(64002, Endpoint::Electrum, false);
    require(replies.ids.last() == 64002 && !replies.results.last().is_ok &&
                row->action_req == connectId,
            "overlap is refused without consuming the active Settings ID");
    EndpointResult late{};
    late.is_ok = true;
    emit backend->endpointRequestFinished(testId, Endpoint::Electrum, late);
    require(row->action_req == connectId, "late earlier completion cannot release current request");
    peer.release();
    waitFor([backend] { return !backend->endpointError(Endpoint::Electrum).isEmpty(); },
            "live Electrum handshake error reaches backend state after dispatch");
    require(row->action_req == 0 && replies.ids.last() == connectId &&
                !backend->endpointOperationsBusy() &&
                !backend->isEndpointRunning(Endpoint::Electrum),
            "actual post-dispatch failure releases busy and leaves backend offline");
    require(!row->field->note()->isHidden() &&
                hasText(*row->field->note(),
                        mapBackendErrorSummary(backend->endpointErrorKind(Endpoint::Electrum)) +
                            QStringLiteral(" ") + backend->endpointError(Endpoint::Electrum)),
            "Settings displays the actual operation failure");
    require(row->field->state() == testState,
            "a connection failure does not change the earlier test result");
    require(toggle->isEnabled() && !toggle->isChecked(),
            "failed Connect restores actual off toggle");

    if (connected.count() > 0) {
        waitFor([&replies] { return replies.ids.count(0) >= 2; },
                "Rust retry policy survives two backend-owned retry executions");
    } else {
        require(!replies.results.last().is_ok,
                "a refused initial handshake fails without arming the connected-only retry policy");
    }
    require(!backend->endpointOperationsBusy() && !backend->isEndpointRunning(Endpoint::Electrum),
            "failed retries finish offline without losing the retry budget after first reopen");

    // The status bar uses the same request/result contract against the now-closed peer.
    peer.close();
    auto previous = replies.ids.size();
    toggle->click();
    require(backend->endpointOperationsBusy() && !row->connect->isEnabled(),
            "status Connect disables Settings through the backend busy state");
    waitFor([&replies, previous] { return replies.ids.size() == previous + 1; },
            "status Connect returns a real failed result");
    require(!replies.results.last().is_ok && !toggle->isChecked() && toggle->isEnabled() &&
                hasText(bar, mapBackendErrorSummary(replies.results.last().kind)) &&
                bar.template findChildren<catalog::StatusBarItem *>().last()->toolTip() ==
                    QString::fromUtf8(replies.results.last().error.data(),
                                      replies.results.last().error.size()),
            "status bar shows the correlated actual error and off state");
    Notification stale{};
    stale.flag = NotificationFlag::ElectrumConnected;
    emit controller.notificationReceived(oldGeneration, stale);
    QApplication::processEvents();
    require(!backend->isEndpointRunning(Endpoint::Electrum),
            "old receiver cannot overwrite newer failed connection state");
    {
        EndpointPeer failedTest(1, true, true);
        auto saved = backend->endpointUrl(Endpoint::Electrum);
        editEndpoint(row->field->input(), failedTest.url());
        row->field->testButton()->click();
        auto failedId = row->action_req;
        waitFor([&failedTest] { return failedTest.waiting(); }, "failed Test reaches real peer");
        require(backend->endpointUrl(Endpoint::Electrum) == saved && !toggle->isEnabled(),
                "candidate Test does not overwrite saved endpoint while pending");
        require(row->field->state() == catalog::EndpointRow::State::Testing &&
                    row->field->testButton()->isHidden() && !row->field->input()->isEnabled(),
                "pending Settings Test presents Testing with the draft locked");
        failedTest.release();
        waitFor([&row] { return row->action_req == 0; }, "failed Test completes");
        require(replies.ids.last() == failedId && !replies.results.last().is_ok &&
                    !replies.results.last().saved &&
                    backend->endpointUrl(Endpoint::Electrum) == saved &&
                    row->field->input()->text() == failedTest.url() &&
                    !row->field->note()->isHidden() &&
                    row->field->state() == catalog::EndpointRow::State::Failed,
                "failed Test keeps saved endpoint, candidate draft and visible failure");
        row->field->setError(QStringLiteral("connection failed"));
        require(row->field->state() == catalog::EndpointRow::State::Failed &&
                    hasText(*row->field->note(), QStringLiteral("connection failed")),
                "a connection error can be shown without replacing the failed test state");
        row->field->setError(QString());
        require(!row->field->note()->isHidden() &&
                    hasText(*row->field->note(),
                            mapBackendErrorSummary(replies.results.last().kind) +
                                QStringLiteral(" ") +
                                QString::fromUtf8(replies.results.last().error.data(),
                                                  replies.results.last().error.size())),
                "clearing a connection error restores the failed test explanation");
        auto revision = row->draft_revision;
        row->ssl->click();
        require(row->field->state() == catalog::EndpointRow::State::Untested &&
                    row->field->note()->isHidden() &&
                    row->field->input()->text() ==
                        QStringLiteral("ssl://") + failedTest.url().mid(6) &&
                    row->draft_revision == revision + 1,
                "SSL toggle turns the failed draft into an untested SSL candidate");
        require(row->verify->isEnabled() && row->verify->isChecked(),
                "an SSL candidate offers certificate verification");
        row->verify->click();
        require(row->field->state() == catalog::EndpointRow::State::Untested &&
                    !row->verify_wanted && row->draft_revision == revision + 2 &&
                    row->field->input()->text() ==
                        QStringLiteral("ssl://") + failedTest.url().mid(6),
                "certificate choice is a new untested draft of the same URL");
    }
    {
        EndpointPeer late(1, false, true);
        editEndpoint(row->field->input(), late.url());
        row->field->testButton()->click();
        auto lateId = row->action_req;
        waitFor([&late] { return late.waiting(); }, "superseded Test reaches real peer");
        require(lateId != 0 && !row->ssl->isChecked() &&
                    row->field->state() == catalog::EndpointRow::State::Testing,
                "plaintext candidate Test is pending");
        // The input is locked while busy: the newer draft arrives as an edit signal.
        auto newer = QStringLiteral("tcp://127.0.0.1:2");
        row->field->input()->setText(newer);
        emit row->field->input()->textEdited(newer);
        require(backend->endpointOperationsBusy() && row->action_req == lateId &&
                    row->field->state() == catalog::EndpointRow::State::Untested,
                "a newer draft supersedes the pending Test while the backend is busy");
        late.release();
        waitFor([&row] { return row->action_req == 0; }, "superseded Test completes");
        require(replies.ids.last() == lateId && replies.results.last().is_ok &&
                    replies.results.last().saved &&
                    backend->endpointUrl(Endpoint::Electrum) == late.url().mid(6) &&
                    row->field->input()->text() == newer &&
                    row->field->state() == catalog::EndpointRow::State::Untested &&
                    row->field->note()->isHidden(),
                "late completion saves its endpoint without overwriting the newer draft");
    }
    controller.stop();
    waitFor([&stopped] { return stopped.count() == 1; }, "interaction fixture drains on shutdown");
}

template <typename Controller, typename Bar> void endpointShutdown(const QString &name) {
    auto config = ::config_from_file(rust::String(name.toStdString()));
    config->set_electrum_url(rust::String());
    require(config->save().empty(), "shutdown fixture opens offline");
    Controller controller(name, nullptr);
    waitFor([&controller] { return !controller.accountLoading(); },
            "shutdown fixture opening finishes");
    AccountBackend *backend = &controller;
    auto *settings = new account::view::Settings(backend);
    Bar bar(&controller);
    EndpointSpy replies;
    SignalSpy stopped;
    QObject::connect(backend, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(&controller, &Controller::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    auto gate = std::make_shared<std::binary_semaphore>(0);
    backend->startWorker(QThread::create([gate] { gate->acquire(); }));
    EndpointPeer peer(1, false, true);
    auto *row = settings->row(Endpoint::Electrum);
    editEndpoint(row->field->input(), peer.url());
    row->field->testButton()->click();
    auto requestId = row->action_req;
    auto oldGeneration = controller.m_notif_generation;
    delete settings;
    controller.stop();
    require(backend->endpointsStopping() && backend->endpointOperationsBusy() &&
                replies.ids == QList<uint64_t>{requestId} && !replies.results.last().is_ok &&
                replies.results.last().error == "Account is stopping" && stopped.count() == 0,
            "view close and shutdown cancel delivery but wait for the owned worker");
    backend->setEndpointConnected(64003, Endpoint::Electrum, true);
    require(replies.ids.last() == 64003 && !replies.results.last().is_ok,
            "all endpoint initiators refuse new work during shutdown");
    gate->release();
    waitFor([&peer] { return peer.waiting(); }, "drained worker reaches its held Test handshake");
    require(stopped.count() == 0,
            "shutdown does not drop Rust account before endpoint worker ends");
    peer.release();
    waitFor([&stopped] { return stopped.count() == 1; },
            "endpoint worker and receiver drain safely");
    require(replies.ids == QList<uint64_t>{requestId, 64003},
            "worker completion after shutdown cannot emit a second success for a cancelled ID");
    Notification late{};
    late.flag = NotificationFlag::ElectrumConnected;
    emit controller.notificationReceived(oldGeneration, late);
    QApplication::processEvents();
    require(!backend->isEndpointRunning(Endpoint::Electrum),
            "late receiver generation cannot revive a stopped account");
}

void blindbitReadsAndShutdown() {
    auto config = ::config_from_file("boundary-sp");
    config->set_electrum_url(rust::String());
    require(config->save().empty(), "BlindBit fixture opens without Electrum");
    AccountController controller(QStringLiteral("boundary-sp"), nullptr);
    waitFor([&controller] { return !controller.accountLoading(); },
            "BlindBit fixture opening finishes");
    AccountBackend *backend = &controller;
    EndpointSpy replies;
    SignalSpy stopped;
    QObject::connect(backend, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(backend, &AccountBackend::endpointInfoReady, &replies, &EndpointSpy::onInfo,
                     qontrol::UNIQUE);
    QObject::connect(&controller, &AccountController::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    {
        EndpointPeer peer(1, false, false, EndpointPeer::Protocol::Blindbit);
        backend->testAndSaveEndpoint(64010, Endpoint::Blindbit, peer.url(), true);
        waitFor([&replies] { return replies.ids.size() == 1; }, "BlindBit Test/save finishes");
        require(replies.results[0].is_ok && replies.results[0].saved &&
                    !backend->isEndpointRunning(Endpoint::Blindbit),
                "BlindBit Test/save retains its actual offline scanner semantics");
    }
    {
        EndpointPeer peer(3, false, true, EndpointPeer::Protocol::Blindbit);
        backend->testAndSaveEndpoint(64011, Endpoint::Blindbit, peer.url(), true);
        waitFor([&replies] { return replies.ids.size() == 2; }, "save live capability peer");
        backend->requestEndpointInfo(64012, Endpoint::Blindbit);
        waitFor([&replies] { return replies.info_ids.size() == 1; },
                "backend-owned info read finishes");
        require(replies.info_ids[0] == 64012 && replies.infos[0].is_ok &&
                    replies.infos[0].height == 123 && replies.infos[0].tweaks_only &&
                    !replies.infos[0].tweaks_full_basic &&
                    replies.infos[0].tweaks_full_with_dust_filter &&
                    !replies.infos[0].tweaks_cut_through_with_dust_filter,
                "BlindBit capabilities and height come from the real backend read");
        auto *settings = new account::view::Settings(backend);
        waitFor([&peer] { return peer.waiting(); },
                "Settings capability request reaches backend worker");
        StatusBar bar(&controller);
        auto saved = backend->endpointUrl(Endpoint::Blindbit);
        backend->setEndpointConnected(64013, Endpoint::Blindbit, false);
        require(backend->endpointOperationsBusy() &&
                    backend->endpointUrl(Endpoint::Blindbit) == saved,
                "endpoint operation retains saved snapshot while draining capability worker");
        delete settings;
        controller.stop();
        require(stopped.count() == 0 && backend->endpointsStopping(),
                "account waits for capability worker after its requesting view closes");
        peer.release();
        waitFor([&stopped] { return stopped.count() == 1; },
                "BlindBit read/command/receiver drain");
        require(replies.info_ids == QList<uint64_t>{64012} && replies.ids.last() == 64013 &&
                    !replies.results.last().is_ok,
                "late capability result is suppressed by generation and shutdown, not sent to "
                "closed view");
    }
}

void caughtUpBlindbitConnection() {
    auto config = ::config_from_file("boundary-sp");
    config->set_electrum_url(rust::String());
    require(config->save().empty(), "caught-up fixture opens offline");
    AccountController controller(QStringLiteral("boundary-sp"), nullptr);
    waitFor([&controller] { return !controller.accountLoading(); }, "caught-up account opens");
    EndpointSpy replies;
    SignalSpy stopped;
    QObject::connect(&controller, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(&controller, &AccountController::notificationReceived, &replies,
                     &EndpointSpy::onNotification, qontrol::UNIQUE);
    QObject::connect(&controller, &AccountController::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    EndpointPeer peer(0, false, true, EndpointPeer::Protocol::CaughtUpBlindbit);
    account::view::Settings settings(&controller);
    StatusBar bar(&controller);
    settings.show();
    auto *row = settings.row(Endpoint::Blindbit);
    editEndpoint(row->field->input(), peer.url());
    row->field->testButton()->click();
    waitFor([&row] { return row->action_req == 0; }, "caught-up BlindBit Test/save completes");
    require(replies.results.last().is_ok && replies.results.last().saved,
            "caught-up server is tested and saved offline");
    row->connect->click();
    auto requestId = row->action_req;
    waitFor(
        [&peer, &replies] {
            return peer.waiting() && replies.notifications.contains(NotificationFlag::StartingScan);
        },
        "real scanner reaches its first blocked height request");
    require(controller.endpointOperationsBusy() &&
                !controller.isEndpointRunning(Endpoint::Blindbit) &&
                !replies.ids.contains(requestId) && !row->disconnect->isEnabled(),
            "StartingScan before the backend height reply is not successful connection completion");
    peer.release();
    waitFor([&row] { return row->action_req == 0; },
            "WaitingForBlocks completes caught-up Connect");
    require(replies.ids.last() == requestId && replies.results.last().is_ok &&
                !replies.results.last().connection_pending &&
                controller.isEndpointRunning(Endpoint::Blindbit) &&
                !controller.endpointOperationsBusy() &&
                controller.endpointError(Endpoint::Blindbit).isEmpty() &&
                row->disconnect->isEnabled(),
            "caught-up Connect finishes with actual live state and usable controls");
    require(replies.notifications.count(NotificationFlag::WaitingForBlocks) == 1 &&
                replies.waiting_height == 0 &&
                !replies.notifications.contains(NotificationFlag::ScanStarted),
            "real caught-up scanner emitted WaitingForBlocks without any ScanStarted or new block");
    row->disconnect->click();
    waitFor([&row] { return row->action_req == 0; }, "caught-up disconnect finishes");
    require(!controller.isEndpointRunning(Endpoint::Blindbit) && row->connect->isEnabled(),
            "intentional disconnect restores Connect");
    auto *toggle =
        bar.findChildren<catalog::StatusBarItem *>().first()->findChild<catalog::Toggle *>();
    toggle->click();
    waitFor(
        [&replies] { return replies.notifications.count(NotificationFlag::WaitingForBlocks) == 2; },
        "status-bar reconnect reaches WaitingForBlocks without mining");
    require(controller.isEndpointRunning(Endpoint::Blindbit) &&
                !controller.endpointOperationsBusy() && toggle->isEnabled() &&
                toggle->isChecked() && replies.waiting_height == 0 &&
                !replies.notifications.contains(NotificationFlag::ScanStarted),
            "no-new-block reconnect finishes successfully rather than remaining permanently busy");
    controller.stop();
    waitFor([&stopped] { return stopped.count() == 1; }, "caught-up scanner shutdown drains");
    auto *widget = new AccountWidget(QStringLiteral("boundary-sp"));
    auto *opening = widget->controller();
    EndpointSpy startup;
    SignalSpy openingStopped;
    QObject::connect(opening, &AccountBackend::endpointRequestFinished, &startup,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(opening, &AccountController::notificationReceived, &startup,
                     &EndpointSpy::onNotification, qontrol::UNIQUE);
    QObject::connect(opening, &AccountController::stopped, &openingStopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    waitFor(
        [opening, &startup] {
            return !opening->accountLoading() && !opening->endpointOperationsBusy() &&
                   opening->isEndpointRunning(Endpoint::Blindbit) && startup.ids.size() == 1;
        },
        "automatic startup also completes through caught-up WaitingForBlocks");
    require(startup.ids[0] == 0 && startup.results[0].is_ok &&
                startup.notifications.contains(NotificationFlag::WaitingForBlocks) &&
                !startup.notifications.contains(NotificationFlag::ScanStarted),
            "startup has one backend completion and needs neither ScanStarted nor a new block");
    opening->stop();
    waitFor([&openingStopped] { return openingStopped.count() == 1; }, "caught-up startup drains");
    delete widget;
}

template <typename Widget, typename Controller>
void asynchronousAccountStartup(const QString &source, const QString &name, const QString &plugin) {
    auto original = ::config_from_file(rust::String(source.toStdString()));
    auto descriptor = original->get_descriptor();
    if (plugin == QStringLiteral("musig")) {
        auto validated = ::validate_musig_descriptor(descriptor);
        require(validated.is_ok, "startup fixture policy is valid");
        descriptor = validated.descriptor;
    }
    EndpointPeer blocked(1, true, true);
    auto config =
        ::new_config(rust::String(name.toStdString()), Network::Signet, descriptor,
                     rust::String("http://127.0.0.1:1"), rust::String(blocked.url().toStdString()),
                     546, rust::String(plugin.toStdString()));
    config->set_taproot_descriptor(original->get_taproot_descriptor());
    config->set_verify_certificate(false);
    require(config->save().empty(), "save real Signet startup fixture with slow saved endpoint");
    auto *widget = new Widget(name);
    Controller *controller = widget->controller();
    EndpointSpy replies;
    SignalSpy stopped;
    QObject::connect(controller, &AccountBackend::endpointRequestFinished, &replies,
                     &EndpointSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(controller, &Controller::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    SignalSpy connected;
    QObject::connect(controller, &Controller::electrumConnected, &connected, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    require(controller->accountLoading() && controller->endpointOperationsBusy() &&
                !controller->hasAccountMetadata() &&
                !controller->screen(QStringLiteral("settings")).has_value(),
            "constructor returns with loading state and no panels built from dummy metadata");
    controller->loadPanel(QStringLiteral("settings"));
    controller->setEndpointConnected(64021, Endpoint::Electrum, true);
    require(replies.ids == QList<uint64_t>{64021} && !replies.results[0].is_ok,
            "initial loading refuses an overlapping endpoint request");
    widget->show();
    SignalSpy pulses;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &pulses, &SignalSpy::onSignal, qontrol::UNIQUE);
    heartbeat.start(0);
    waitFor([controller] { return !controller->accountLoading(); },
            "initial offline opening finishes before connecting");
    waitFor(
        [controller, &blocked, &pulses] {
            return blocked.waiting() && !controller->accountLoading() && pulses.count() >= 2;
        },
        "GUI events run while the real initial header request is blocked");
    heartbeat.stop();
    require(controller->hasAccountMetadata() && controller->network() == Network::Signet &&
                std::string(controller->getDescriptor()) == std::string(descriptor) &&
                controller->endpointOperationsBusy() &&
                !controller->isEndpointRunning(Endpoint::Electrum),
            "real network and descriptor survive worker ownership without fallback Regtest or "
            "empty data");
    auto *settings = widget->template findChild<account::view::Settings *>();
    require(settings != nullptr && settings->isVisible() &&
                hasText(*settings, TR("network-signet")),
            "early Settings navigation is retained and constructed with actual metadata");
    auto *row = settings->row(Endpoint::Electrum);
    require(
        row != nullptr && row->saved_url == blocked.url() && !controller->verifiesCertificate() &&
            !row->field->input()->isEnabled() && replies.ids.size() == 1,
        "saved endpoint and certificate choice are present during startup, before any completion");
    blocked.release();
    waitFor(
        [controller, &replies] {
            return !controller->endpointOperationsBusy() && replies.ids.size() == 2 &&
                   !controller->isEndpointRunning(Endpoint::Electrum) &&
                   !controller->endpointError(Endpoint::Electrum).isEmpty();
        },
        "failed initial header request completes through the backend");
    require(
        replies.ids.last() == 0 && (!replies.results.last().is_ok || connected.count() > 0) &&
            controller->getAccount().has_value() && controller->getAccount().value()->is_ok() &&
            controller->network() == Network::Signet &&
            std::string(controller->getDescriptor()) == std::string(descriptor) &&
            row->field->input()->isEnabled() && !row->field->note()->isHidden(),
        "failed startup retains the named offline account and usable Settings with real metadata");
    {
        EndpointPeer repaired;
        editEndpoint(row->field->input(), repaired.url());
        row->field->testButton()->click();
        auto repairId = row->action_req;
        waitFor([&row] { return row->action_req == 0; },
                "Settings repairs initial endpoint failure");
        require(repairId != 0 && replies.ids.last() == repairId && replies.results.last().is_ok &&
                    replies.results.last().saved && !replies.results.last().connection_pending &&
                    controller->endpointError(Endpoint::Electrum).isEmpty() &&
                    row->field->note()->isHidden() && row->connect->isEnabled() &&
                    controller->network() == Network::Signet,
                "fresh public Test/save succeeds after startup failure without losing identity or "
                "metadata");
    }
    controller->stop();
    waitFor([&stopped] { return stopped.count() == 1; }, "startup fixture drains on shutdown");
    delete widget;

    // Close during offline opening, before panels or endpoints can start.
    auto *closing = new Widget(name);
    auto *closingController = closing->controller();
    SignalSpy openingStopped;
    QObject::connect(closingController, &Controller::stopped, &openingStopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    require(closingController->accountLoading(), "second instance is still opening");
    closingController->stop();
    waitFor([&openingStopped] { return openingStopped.count() == 1; },
            "shutdown drains opening worker without starting endpoints or building stale panels");
    require(!closingController->screen(QStringLiteral("settings")).has_value(),
            "late opening cannot populate a shutting-down account");
    delete closing;
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

namespace {
void prepareEndpointAccounts() {
    ::app_set_last_endpoint(BackendServer::Blindbit, Network::Regtest, "http://127.0.0.1:1");
    ::app_set_last_endpoint(BackendServer::Electrum, Network::Regtest, "invalid-endpoint");
    AppController::get()->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    AppController::get()->registerBuiltinPlugin(std::make_unique<MusigPlugin>());
    AppController::get()->registerBuiltinPlugin(std::make_unique<keys::KeysModule>());
    SignalSpy launcher;
    QObject::connect(Host::get()->events(), &HostEvents::launcherRefreshRequested, &launcher,
                     &SignalSpy::onSignal, qontrol::UNIQUE);
    auto key = softwareKey();
    spCreation(key, launcher);
    policyCreation(key, launcher);
    auto config = ::config_from_file("boundary-sp");
    config->set_taproot_descriptor(rust::String(key.policy.toStdString()));
    config->set_electrum_url(rust::String());
    require(config->save().empty(), "SP endpoint fixture has a real taproot sub-account");
    auto policy = ::config_from_file("boundary-policy");
    policy->set_electrum_url(rust::String());
    require(policy->save().empty(), "MuSig endpoint fixture starts offline");
}
} // namespace

void navigationSmoke() {
    prepareEndpointAccounts();
    accountNavigationSmoke();
}

void endpointSmoke() {
    prepareEndpointAccounts();
    savedEndpointObservers<AccountController>(QStringLiteral("boundary-sp"));
    savedEndpointObservers<MusigAccountController>(QStringLiteral("boundary-policy"));
#ifdef Q_OS_UNIX
    endpointInteractions<AccountController, StatusBar>(QStringLiteral("boundary-sp"));
    endpointInteractions<MusigAccountController, MusigStatusBar>(QStringLiteral("boundary-policy"));
    endpointShutdown<AccountController, StatusBar>(QStringLiteral("boundary-sp"));
    endpointShutdown<MusigAccountController, MusigStatusBar>(QStringLiteral("boundary-policy"));
    blindbitReadsAndShutdown();
    caughtUpBlindbitConnection();
    asynchronousAccountStartup<AccountWidget, AccountController>(
        QStringLiteral("boundary-sp"), QStringLiteral("startup-sp"), QStringLiteral("sp"));
    asynchronousAccountStartup<MusigAccountWidget, MusigAccountController>(
        QStringLiteral("boundary-policy"), QStringLiteral("startup-policy"),
        QStringLiteral("musig"));
#endif
}

namespace {

void copyDeletionAccount(const QString &source, const QString &name) {
    auto original = ::config_from_file(rust::String(source.toStdString()));
    require(original->is_ok(), "deletion source account exists");
    auto config = ::new_config(rust::String(name.toStdString()), original->get_network(),
                               original->get_descriptor(), original->get_blindbit_url(), {},
                               original->get_dust_limit(), original->get_plugin_id());
    config->set_segwit_descriptor(original->get_segwit_descriptor());
    config->set_taproot_descriptor(original->get_taproot_descriptor());
    require(config->save().empty(), "isolated logical deletion account is saved");
}

auto sharedDeletionState() -> QMap<QString, QByteArray> {
    QMap<QString, QByteArray> files;
    auto root = QDir::homePath() + QStringLiteral("/.silent");
    for (const auto &folder : {QStringLiteral("keys"), QStringLiteral("musig-sessions")}) {
        QDirIterator entries(root + QLatin1Char('/') + folder, QDir::Files,
                             QDirIterator::Subdirectories);
        while (entries.hasNext()) {
            auto path = entries.next();
            QFile file(path);
            require(file.open(QIODevice::ReadOnly), "shared key/session fixture reads");
            files.insert(path, file.readAll());
        }
    }
    QFile unlock(root + QStringLiteral("/unlock.key"));
    require(unlock.open(QIODevice::ReadOnly), "original application unlock key exists");
    files.insert(unlock.fileName(), unlock.readAll());
    return files;
}

void deletionConfirmation(MainWindow &window, IModule *module) {
    auto name = QStringLiteral("delete-confirmation");
    copyDeletionAccount("boundary-sp", name);
    auto *registry = AppController::get()->pluginRegistry();
    registry->relistModules();
    auto *launcher = window.findChild<MenuTab *>();
    require(launcher != nullptr, "the real launcher exists");
    window.showTab(launcher);
    auto trash = [&window, &name]() -> QPushButton * {
        for (auto *button : window.findChildren<QPushButton *>()) {
            if (button->property("accountName").toString() == name) {
                return button;
            }
        }
        return nullptr;
    };
    waitFor([&trash] { return trash() != nullptr; },
            "generic launcher exposes the account trash action");
    DeleteConfirmationDriver driver(window);
    DeletionSpy results;
    QObject::connect(registry, &PluginRegistry::accountDeletionFinished, &results,
                     &DeletionSpy::onFinished, qontrol::UNIQUE);
    trash()->click();
    require(driver.seen() == 1 && driver.text().contains(name) && results.ids.empty(),
            "the unchanged confirmation shows the stable identity and cancel dispatches nothing");
    require(::config_from_file(rust::String(name.toStdString()))->is_ok(),
            "cancel preserves persisted account data");
    driver.confirmDelete();
    trash()->click();
    waitFor([&results] { return results.ids.size() == 1; },
            "confirmed UI deletion completes through its owning module");
    require(driver.seen() == 2 && results.ids.first() == name && results.errors.first().isEmpty(),
            "one explicit confirmation produces one successful correlated result");
    require(!::config_from_file(rust::String(name.toStdString()))->is_ok(),
            "confirmed account was removed");
    auto listed = registry->launcherInstancesForModule(module);
    require(
        std::ranges::none_of(listed, [&name](const auto &entry) { return entry.first == name; }),
        "the UI result refreshes the owning module's discovery only after success");
}

template <typename Controller>
auto deletionController(MainWindow &window, const QString &name) -> Controller * {
    for (auto *controller : window.findChildren<Controller *>()) {
        if (controller->property("logicalAccountId").toString() == name) {
            return controller;
        }
    }
    return nullptr;
}

template <typename Controller>
void deletionCycle(MainWindow &window, IModule *module, const QString &source, const QString &name,
                   bool close_first, bool wait_closed) {
    copyDeletionAccount(source, name);
    auto *registry = AppController::get()->pluginRegistry();
    registry->relistModules();
    require(registry->startLauncherInstance(module, name), "real account starts through registry");
    auto *controller = deletionController<Controller>(window, name);
    require(controller != nullptr, "real module-owned controller is tracked");
    waitFor(
        [controller] {
            return !controller->accountLoading() && !controller->endpointOperationsBusy();
        },
        "real backend finishes opening and its offline startup transition");
    require(controller->getAccount().has_value() && controller->getAccount().value()->is_ok(),
            "real isolated backend opened");
    std::binary_semaphore release(0);
    std::atomic<bool> entered{false};
    std::atomic<bool> written{false};
    auto *account = &*controller->getAccount().value();
    auto *worker = QThread::create([account, &release, &entered, &written] {
        entered = true;
        release.acquire();
        auto result = account->update_coin_label(rust::String(std::string(64, '1') + ":0"),
                                                 rust::String("delayed deletion label"));
        require(result.is_ok, "real delayed label write succeeds before removal");
        if constexpr (requires { account->retry_saved_psbts(); }) {
            auto saved = account->retry_saved_psbts();
            require(saved.is_ok && saved.save_error.empty(),
                    "delayed real saved-PSBT persistence completes before removal");
        }
        written = true;
    });
    controller->startWorker(worker);
    waitFor([&entered] { return entered.load(); }, "delayed real write entered");
    if constexpr (requires { controller->retrySavedPsbts(); }) {
        controller->retrySavedPsbts();
        require(!controller->m_storage_queue.empty(),
                "public saved-PSBT retry is accepted in the controller queue");
    }
    SignalSpy stopped;
    QObject::connect(controller, &Controller::stopped, &stopped, &SignalSpy::onSignal,
                     qontrol::UNIQUE);
    if (close_first) {
        Host::get()->removeInstance(name, module);
        require(::config_from_file(rust::String(name.toStdString()))->is_ok(),
                "ordinary tab close preserves config while backend is closing");
    }
    if (wait_closed) {
        release.release();
        waitFor([&stopped] { return stopped.count() == 1; }, "closed backend destruction barrier");
        require(written.load(), "closed account flushed delayed work");
    }
    DeletionSpy results;
    QObject::connect(registry, &PluginRegistry::accountDeletionFinished, &results,
                     &DeletionSpy::onFinished, qontrol::UNIQUE);
    QPointer<IAccount> publicAccount;
    if (!close_first) {
        for (auto *instance : dynamic_cast<HostImpl *>(Host::get())->liveInstances()) {
            if (instance->id() == name) {
                publicAccount = instance->account();
            }
        }
        require(publicAccount != nullptr, "the real account exposes its public capability");
        QObject::connect(publicAccount, &IAccount::error, &results, &DeletionSpy::onAccountError,
                         qontrol::UNIQUE);
    }
    registry->deleteInstance(module, name);
    if (publicAccount != nullptr) {
        auto coins = publicAccount->coins();
        auto balance = publicAccount->balance();
        require(!results.account_errors.value(coins).isEmpty() &&
                    !results.account_errors.value(balance).isEmpty(),
                "pending deletion refuses new public account reads, not dummy data");
    }
    require(results.ids.empty(), "delete dispatch is not a success result");
    if (!wait_closed) {
        waitFor([controller] { return controller->endpointsStopping(); },
                "owning module requests asynchronous shutdown");
        require(results.ids.empty() && !written.load(), "deletion waits for the actual writer");
        require(::config_from_file(rust::String(name.toStdString()))->is_ok(),
                "storage remains until all workers and backend destruction finish");
        require(!registry->startLauncherInstance(module, name), "pending deletion blocks reopen");
        std::atomic<bool> lateWorker{false};
        controller->startWorker(QThread::create([&lateWorker] { lateWorker = true; }));
        QApplication::processEvents();
        require(!lateWorker.load(), "pending deletion refuses a new public worker");
        release.release();
    }
    waitFor([&results] { return results.ids.size() == 1; }, "persisted deletion completes");
    require(results.ids.first() == name && results.errors.first().isEmpty(),
            "one correlated successful deletion result");
    require(written.load(), "outstanding persistence preceded deletion");
    require(!::config_from_file(rust::String(name.toStdString()))->is_ok(),
            "logical account removed");
    require(::config_from_file(rust::String(source.toStdString()))->is_ok(),
            "another account and its logical files survive");
    QThread::msleep(80);
    QApplication::processEvents();
    require(!::config_from_file(rust::String(name.toStdString()))->is_ok(),
            "no late worker recreates removed account state");
}

#ifdef Q_OS_UNIX
template <typename Controller>
void deletionShutdownFailure(MainWindow &window, IModule *module, const QString &source,
                             const QString &name, const QString &plugin) {
    copyDeletionAccount(source, name);
    auto *registry = AppController::get()->pluginRegistry();
    registry->relistModules();
    require(registry->startLauncherInstance(module, name), "shutdown failure account starts");
    auto *controller = deletionController<Controller>(window, name);
    require(controller != nullptr, "shutdown failure uses the real controller");
    waitFor(
        [controller] {
            return !controller->accountLoading() && !controller->endpointOperationsBusy();
        },
        "failure fixture opens offline");
    std::binary_semaphore release(0);
    std::atomic<bool> entered{false};
    std::atomic<bool> failed{false};
    auto *account = &*controller->getAccount().value();
    controller->startWorker(QThread::create([account, &release, &entered, &failed] {
        entered = true;
        release.acquire();
        auto result =
            account->update_coin_label(rust::String(std::string(64, '2') + ":0"), "unsaved label");
        failed = !result.is_ok && !result.error.empty();
    }));
    waitFor([&entered] { return entered.load(); }, "failure writer is pending");
    DeletionSpy results;
    SignalSpy refresh;
    QObject::connect(registry, &PluginRegistry::accountDeletionFinished, &results,
                     &DeletionSpy::onFinished, qontrol::UNIQUE);
    QObject::connect(registry, &PluginRegistry::enabledPluginsChanged, &refresh,
                     &SignalSpy::onSignal, qontrol::UNIQUE);
    registry->deleteInstance(module, name);
    waitFor([controller] { return controller->endpointsStopping(); },
            "deletion passed validation and waits for shutdown");
    auto root = QDir::homePath() + QStringLiteral("/.silent/") + plugin;
    auto permissions = QFile::permissions(root);
    require(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::ExeOwner),
            "deny actual shutdown persistence");
    release.release();
    waitFor([&results] { return results.ids.size() == 1; },
            "failed persistence produces deletion result");
    require(QFile::setPermissions(root, permissions), "restore storage after shutdown failure");
    require(failed.load() && !results.errors.first().isEmpty(),
            "pending persistence failure prevents persisted deletion");
    require(refresh.count() == 0,
            "failure does not refresh launcher discovery as successful deletion");
    require(::config_from_file(rust::String(name.toStdString()))->is_ok(),
            "shutdown failure preserves the account's persisted data");
    auto listed = registry->launcherInstancesForModule(module);
    require(std::ranges::any_of(listed, [&name](const auto &item) { return item.first == name; }),
            "failed deletion remains in registry discovery");
}
#endif

} // namespace

void deletionSmoke() {
    AppController::get()->initState();
    MainWindow window;
    AppController::get()->start(&window);
    prepareEndpointAccounts();
    window.resize(qEnvironmentVariableIsEmpty("SILENT_DELETION_SHOTS") ? 1440 : 1920,
                  qEnvironmentVariableIsEmpty("SILENT_DELETION_SHOTS") ? 1000 : 1080);
    window.show();
    auto *sp = SpModule::get();
    auto *musig = MusigModule::get();
    require(sp != nullptr && musig != nullptr, "both owning modules exist");
    auto history = QDir::homePath() + QStringLiteral("/.silent/musig-sessions");
    require(QDir().mkpath(history), "isolated shared signing-history boundary exists");
    QFile sentinel(history + QStringLiteral("/deletion-history-sentinel"));
    require(sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                sentinel.write("shared history must survive account deletion") > 0,
            "nonempty shared history sentinel is installed");
    sentinel.close();
    auto shared = sharedDeletionState();
    deletionCycle<AccountController>(window, sp, "boundary-sp", "delete-sp-open", false, false);
    deletionCycle<AccountController>(window, sp, "boundary-sp", "delete-sp-closing", true, false);
    deletionCycle<AccountController>(window, sp, "boundary-sp", "delete-sp-closed", true, true);
    deletionCycle<MusigAccountController>(window, musig, "boundary-policy", "delete-musig-open",
                                          false, false);
    deletionCycle<MusigAccountController>(window, musig, "boundary-policy", "delete-musig-closing",
                                          true, false);
    deletionCycle<MusigAccountController>(window, musig, "boundary-policy", "delete-musig-closed",
                                          true, true);
    deletionConfirmation(window, sp);
    auto *registry = AppController::get()->pluginRegistry();
    DeletionSpy results;
    DeletionErrorObserver errors(window);
    QObject::connect(registry, &PluginRegistry::accountDeletionFinished, &results,
                     &DeletionSpy::onFinished, qontrol::UNIQUE);
    registry->deleteInstance(musig, "boundary-sp");
    require(results.ids.size() == 1 && !results.errors.first().isEmpty(),
            "cross-module identity is refused without touching storage");
    ::state_lock();
    registry->deleteInstance(sp, "boundary-sp");
    waitFor([&results] { return results.ids.size() == 2; }, "locked deletion answers");
    require(!results.errors.last().isEmpty(), "locked storage is an explicit failure");
    require(::state_unlock_with_password("host smoke").empty(), "same app unlock key is retained");
    require(::config_from_file("boundary-sp")->is_ok(), "failed deletion stays discoverable");
    require(errors.messages().size() == 2 &&
                errors.messages().first().contains(QStringLiteral("identity")) &&
                errors.messages().last().contains(QStringLiteral("locked")),
            "the real generic shell displays both explicit deletion failures");
#ifdef Q_OS_UNIX
    auto root = QDir::homePath() + QStringLiteral("/.silent/sp");
    auto permissions = QFile::permissions(root);
    require(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::ExeOwner),
            "make isolated plugin storage read-only");
    registry->deleteInstance(sp, "boundary-sp");
    waitFor([&results] { return results.ids.size() == 3; }, "I/O deletion failure answers");
    require(!results.errors.last().isEmpty(), "I/O failure is not deletion success");
    require(QFile::setPermissions(root, permissions), "restore isolated storage");
    require(::config_from_file("boundary-sp")->is_ok(), "I/O failure leaves account discoverable");
#endif
#ifdef Q_OS_UNIX
    deletionShutdownFailure<AccountController>(window, sp, "boundary-sp", "delete-sp-failed-flush",
                                               "sp");
    deletionShutdownFailure<MusigAccountController>(window, musig, "boundary-policy",
                                                    "delete-musig-failed-flush", "musig");
#endif
    require(sharedDeletionState() == shared, "deleting one account preserves shared keys, session "
                                             "history and the exact application unlock key bytes");
}

} // namespace smoke

#include "boundary_smoke.moc"
