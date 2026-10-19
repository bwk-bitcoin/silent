// Device GUI end to end (tests/e2e/devices/run.sh): the real silent app on a private
// Xvfb display (QT_QPA_PLATFORM=xcb) against a regtest chain (the e2e_chain example),
// a Ledger on Speculos (key B) and the Coldcard simulator (key C), with key A hot in
// silent. A 2-of-3 MuSig2 shared wallet is created, registered on both devices,
// funded, and spent twice: through the Ledger + Coldcard leaf (with a lost session,
// the Coldcard simulator restarted between round 1 and round 2) and through the
// Ledger + hot leaf. The Ledger is approved by ledger_clicker.py through the Speculos
// REST API, the Coldcard by coldcard_driver.py through the simulator's key input.
// Every shot is the window area grabbed from the screen, any modal over it included.
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <QtWidgets>
#define private public
#define protected public
#include "AppController.h"
#include "MainWindow.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/Modal.h"
#include "catalog/containers/Tabs.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/MessageModal.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/EndpointRow.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "catalog/panels/receive/AddressRow.h"
#include "i18n/I18nManager.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "interfaces/module.h"
#include "interfaces/signer.h"
#include "interfaces/types.h"
#include "metatypes.h"
#include "modules/hwi/HwiPlugin.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysTab.h"
#include "modules/keys/flow/SigningFlow.h"
#include "modules/keys/flow/signingflow/UsbRouteFlow.h"
#include "modules/keys/modals/KeyDetail.h"
#include "modules/keys/modals/RegisterDevice.h"
#include "modules/keys/modals/Unseal.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/musig/creator/CreatePolicy.h"
#include "modules/musig/RegisterDescriptor.h"
#include "modules/musig/SettingsExtras.h"
#include "account/views/Settings.h"
#include "modules/musig/SignModal.h"
#include "modules/musig/SigningSection.h"
#include "modules/sp/SpPlugin.h"
#include "qr/QrTypes.h"
#include "resources/font/noto_sans.h"
#include "resources/font/noto_sans_bold.h"
#include "resources/font/noto_sans_mono.h"
#include "resources/font/noto_sans_mono_bold.h"
#include "theme/Theme.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "catalog/panels/receive/AddressRow.h"
#include "views/modals/AccountType.h"
#undef private
#undef protected
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <common.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <sys/types.h>
#include <unistd.h>

using keys::flow::SigningFlow;
using keys::flow::UsbRouteFlow;
using musig::modal::CreatePolicy;

namespace {

const QString ACCOUNT = QStringLiteral("shared");
const QString PASSWORD = QStringLiteral("pw");
const QString LEDGER_ID = QStringLiteral("ledger-simulator");
const QString COLDCARD_ID = QStringLiteral("coldcard-simulator");
constexpr int TICK_MS = 300;
// Device approvals wait on a person in real use: a step may take this long.
constexpr qint64 STEP_TIMEOUT_MS = 240000;
constexpr int CHAIN_START_TIMEOUT_MS = 180000;
constexpr int CHAIN_ANSWER_TIMEOUT_MS = 60000;
constexpr int SHOT_SETTLE_MS = 900;
constexpr int SIM_STOP_TIMEOUT_MS = 5000;
constexpr int SIM_SOCKET_TIMEOUT_S = 90;
constexpr int APPROVE_TIMEOUT_S = 120;
constexpr int WINDOW_WIDTH = 1920;
constexpr int WINDOW_HEIGHT = 1080;
constexpr int EXIT_TIMEOUT = 2;
constexpr int BECH32_CHECKSUM_LENGTH = 6;

// The probe's inputs, from run.sh.
struct Env {
    QString run_dir;
    QString chain_bin;
    QString coldcard_unix_dir;
    QString python;
    QString drivers_dir;
    QString ledger_screens;
    QString seed_a;
    QString seed_c;
    QString coldcard_socket;
};

Env g_env;
MainWindow *g_window = nullptr;
keys::KeysModule *g_keys = nullptr;
int g_failures = 0;
QString g_fp_a;
QString g_fp_b;
QString g_fp_c;
QString g_electrum;
QString g_descriptor;
QStringList g_txids;
// B and C in the order the creator registers them.
QStringList g_register_devices;
QString g_last_error;
bool g_broadcast_answered = false;
qsizetype g_instances_before = 0;
// Where the Ledger screens log stood when a check on it started.
qint64 g_screens_mark = 0;

void check(bool ok, const QString &what) {
    std::printf("%s: %s\n", ok ? "OK  " : "FAIL", what.toStdString().c_str());
    if (!ok) {
        ++g_failures;
    }
}

void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

auto requireEnv(const char *name) -> QString {
    auto value = qEnvironmentVariable(name);
    if (value.isEmpty()) {
        std::fprintf(stderr, "device_e2e: set %s (run tests/e2e/devices/run.sh)\n", name);
        std::exit(1);
    }
    return value;
}

// ===== Line process: the chain =====

// A child answering one line per command line.
class LineProcess {
public:
    LineProcess(const QString &program, const QString &stderr_log) {
        m_process.setProgram(program);
        m_process.setProcessChannelMode(QProcess::SeparateChannels);
        m_process.setStandardErrorFile(stderr_log, QIODevice::Append);
    }
    auto start(int timeout_ms) -> QString {
        m_process.start();
        return readLine(timeout_ms);
    }
    auto ask(const QString &line, int timeout_ms) -> QString {
        m_process.write((line + "\n").toUtf8());
        m_process.waitForBytesWritten(timeout_ms);
        return readLine(timeout_ms);
    }
    // `quit` has no answer: waits for the child to end instead.
    void quit(int timeout_ms) {
        m_process.write("quit\n");
        m_process.waitForBytesWritten(timeout_ms);
        if (!m_process.waitForFinished(timeout_ms)) {
            m_process.kill();
            m_process.waitForFinished(timeout_ms);
        }
    }

private:
    auto readLine(int timeout_ms) -> QString {
        QElapsedTimer t;
        t.start();
        while (!m_process.canReadLine() && t.elapsed() < timeout_ms) {
            m_process.waitForReadyRead(200);
        }
        return QString::fromUtf8(m_process.readLine()).trimmed();
    }
    QProcess m_process;
};

LineProcess *g_chain = nullptr;

auto chain(const QString &command) -> QString {
    auto answer = g_chain->ask(command, CHAIN_ANSWER_TIMEOUT_MS);
    std::printf("chain %s -> %s\n", command.toStdString().c_str(), answer.toStdString().c_str());
    return answer.startsWith("OK ") ? answer.mid(3) : QString();
}

// ===== Coldcard simulator =====

void ownProcessGroup() {
    ::setpgid(0, 0);
}

// The simulator in its own process group, killed by group, as bwk-hwi's SimProcess
// (hwi/tests/common/coldcard_sim.rs) does, on the fixed socket path the hwi service
// watches (no --segregate).
class ColdcardSim {
public:
    void start() {
        m_process = std::make_unique<QProcess>();
        m_process->setProgram(g_env.python);
        m_process->setArguments({"simulator.py", "--eff", "--headless", "--seed", g_env.seed_c});
        m_process->setWorkingDirectory(g_env.coldcard_unix_dir);
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("SDL_VIDEODRIVER", "dummy");
        m_process->setProcessEnvironment(env);
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        m_process->setStandardInputFile(QProcess::nullDevice());
        m_process->setStandardOutputFile(g_env.run_dir + "/coldcard-sim.log", QIODevice::Append);
        m_process->setChildProcessModifier(ownProcessGroup);
        m_process->start();
        check(m_process->waitForStarted(), "Coldcard simulator started");
        m_pid = m_process->processId();
        QFile pid_file(g_env.run_dir + "/coldcard-sim.pid");
        if (pid_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pid_file.write(QByteArray::number(m_pid) + "\n");
        }
        std::printf("Coldcard simulator pid %lld\n", static_cast<long long>(m_pid));
    }

    // TERM to the group, KILL after 5 s, then the socket and the simulator's work
    // directory are removed, so the hwi service sees the socket go.
    void stop() {
        if (m_process == nullptr) {
            return;
        }
        ::kill(static_cast<pid_t>(-m_pid), SIGTERM);
        if (!m_process->waitForFinished(SIM_STOP_TIMEOUT_MS)) {
            ::kill(static_cast<pid_t>(-m_pid), SIGKILL);
            m_process->waitForFinished(SIM_STOP_TIMEOUT_MS);
        }
        // the simulator's own children (micropython) share the group
        ::kill(static_cast<pid_t>(-m_pid), SIGKILL);
        QFile::remove(g_env.coldcard_socket);
        QDir(QStringLiteral("/tmp/cc-simulators/%1").arg(m_pid)).removeRecursively();
        QFile::remove(g_env.run_dir + "/coldcard-sim.pid");
        m_process.reset();
    }

    [[nodiscard]] auto socketPresent() const -> bool {
        return QFileInfo(g_env.coldcard_socket).exists();
    }

private:
    std::unique_ptr<QProcess> m_process;
    qint64 m_pid = 0;
};

ColdcardSim g_coldcard;

// One coldcard_driver.py command at a time, run without blocking the event loop and
// polled by the steps.
class ColdcardDriver {
public:
    void start(const QStringList &args) {
        m_process = std::make_unique<QProcess>();
        m_process->setProgram(g_env.python);
        m_process->setArguments(QStringList{"-I", g_env.drivers_dir + "/coldcard_driver.py", "--socket",
                                            g_env.coldcard_socket} +
                                args);
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        m_process->start();
        std::printf("coldcard_driver %s\n", args.join(' ').toStdString().c_str());
    }
    [[nodiscard]] auto finished() const -> bool {
        return m_process != nullptr && m_process->state() == QProcess::NotRunning;
    }
    // The command's exit code and output, once finished.
    auto result() -> std::pair<int, QString> {
        auto output = QString::fromUtf8(m_process->readAll()).trimmed();
        std::printf("coldcard_driver -> %d: %s\n", m_process->exitCode(),
                    output.toStdString().c_str());
        return {m_process->exitCode(), output};
    }

private:
    std::unique_ptr<QProcess> m_process;
};

ColdcardDriver g_cc_driver;
// Since the last Electrum event the run caused.
QElapsedTimer g_since;
// A ping every 20 s, unanswered after 5 s: a dead link is caught within this.
constexpr qint64 KEEPALIVE_DETECT_MS = 40000;
// The relative timelock, in blocks, of the Coldcard's recovery path.
constexpr int RECOVERY_BLOCKS = 3;
// Since the wizard sent the Coldcard its enroll request.
QElapsedTimer g_enroll_sent;
// How long the Coldcard is left unapproved before the check.
constexpr qint64 ENROLL_WAIT_MS = 3000;

void approveColdcard() {
    g_cc_driver.start({"approve", "--timeout", QString::number(APPROVE_TIMEOUT_S)});
}

void dismissColdcard() {
    g_cc_driver.start({"dismiss"});
}

// ===== Ledger screens =====

// The screens ledger_clicker.py logged since the mark, one per line, timestamp dropped.
auto ledgerScreensSinceMark() -> QStringList {
    QFile file(g_env.ledger_screens);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    file.seek(g_screens_mark);
    QStringList screens;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts)) {
        screens.append(line.section(' ', 1));
    }
    return screens;
}

void markLedgerScreens() {
    g_screens_mark = QFileInfo(g_env.ledger_screens).size();
}

// Every screen's text after its title, joined without separators or spaces: an
// address split over several screens reads whole.
auto ledgerScreensText() -> QString {
    QString text;
    for (const auto &screen : ledgerScreensSinceMark()) {
        auto parts = screen.split(" | ");
        parts.removeFirst();
        text += parts.join(QString());
    }
    return text.remove(' ');
}

// ===== Window helpers =====

int g_shot_count = 0;

void shot(const QString &name) {
    spin(SHOT_SETTLE_MS);
    auto g = g_window->frameGeometry();
    auto pixmap = QGuiApplication::primaryScreen()->grabWindow(0, g.x(), g.y(), g.width(),
                                                                g.height());
    QString path = g_env.run_dir + "/shots/" + name + ".png";
    bool saved = pixmap.save(path);
    ++g_shot_count;
    std::printf("shot %s %s\n", path.toStdString().c_str(), saved ? "saved" : "NOT SAVED");
}

template <typename T> auto visible() -> T * {
    for (auto *w : QApplication::topLevelWidgets()) {
        if (auto *m = qobject_cast<T *>(w); m != nullptr && m->isVisible()) {
            return m;
        }
    }
    return nullptr;
}

auto launcherButton(const QString &text, bool exact) -> QPushButton * {
    for (auto *button : g_window->findChildren<QPushButton *>()) {
        if (!button->isVisible()) {
            continue;
        }
        auto t = button->text().trimmed();
        if (exact ? t == text : t.contains(text)) {
            return button;
        }
    }
    return nullptr;
}

auto signerById(const QString &id) -> std::optional<plugin::SignerEntry> {
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.signer->id() == id) {
            return entry;
        }
    }
    return std::nullopt;
}

// The status of `fp`'s row in the signer picker on display, NoRoute when none shows.
auto pickerStatus(const QString &fp) -> keys::flow::SignerStatus {
    auto *f = visible<SigningFlow>();
    if (f == nullptr || f->m_flow != nullptr) {
        return keys::flow::SignerStatus::NoRoute;
    }
    for (const auto &row : f->m_rows) {
        if (QString::compare(row.fingerprint, fp, Qt::CaseInsensitive) == 0) {
            return row.status;
        }
    }
    return keys::flow::SignerStatus::NoRoute;
}

auto signerReady(const QString &id) -> bool {
    auto entry = signerById(id);
    return entry.has_value() && entry->signer->state() == plugin::SignerState::Ready &&
           !entry->signer->fingerprint().isEmpty();
}

auto tabKey(const QString &fp) -> std::optional<KeyInfo> {
    for (const auto &info : g_keys->tabKeys()) {
        if (QString::compare(keys::KeysModule::toQt(info.fingerprint), fp, Qt::CaseInsensitive) ==
            0) {
            return info;
        }
    }
    return std::nullopt;
}

auto hasUsbRoute(const QString &fp) -> bool {
    auto info = tabKey(fp);
    if (!info.has_value()) {
        return false;
    }
    for (const auto &route : info->routes) {
        if (route == KeyRouteKind::Usb) {
            return true;
        }
    }
    return false;
}

auto keysTab() -> keys::KeysTab * {
    return g_window->findChild<keys::KeysTab *>();
}

// The Keys tab row of a live device, or of a key.
auto keysTabRow(const char *property, const QString &value) -> catalog::SelectRow * {
    auto *tab = keysTab();
    if (tab == nullptr) {
        return nullptr;
    }
    for (auto *row : tab->findChildren<catalog::SelectRow *>()) {
        if (QString::compare(row->property(property).toString(), value, Qt::CaseInsensitive) ==
            0) {
            return row;
        }
    }
    return nullptr;
}

auto flowRow(SigningFlow *flow, const QString &fp) -> int {
    for (int i = 0; i < flow->m_rows.size(); ++i) {
        if (QString::compare(flow->m_rows.at(i).fingerprint, fp, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

// A USB route of `fp` waiting on its device.
auto usbWaiting(const QString &fp) -> bool {
    auto *f = visible<SigningFlow>();
    auto *usb = f == nullptr ? nullptr : qobject_cast<UsbRouteFlow *>(f->m_flow);
    return usb != nullptr &&
           QString::compare(usb->row().fingerprint, fp, Qt::CaseInsensitive) == 0 &&
           usb->m_device_req != 0;
}

auto unsealShown() -> keys::modal::Unseal * {
    auto *u = visible<keys::modal::Unseal>();
    if (u != nullptr && u->m_view == keys::modal::Unseal::View::Password) {
        return u;
    }
    return nullptr;
}

auto widget() -> MusigAccountWidget * {
    return g_window->findChild<MusigAccountWidget *>();
}

// The account's Electrum connection is up.
auto electrumUp() -> bool {
    return widget() != nullptr && widget()->controller()->isEndpointRunning(Endpoint::Electrum);
}

auto sendScreen() -> account::view::Send * {
    auto *w = widget();
    return w == nullptr ? nullptr : w->findChild<account::view::Send *>();
}

// The Multikey signing step of Send: the spend path, Sign, Cancel.
auto section() -> musig::SigningSection * {
    auto *w = widget();
    return w == nullptr ? nullptr : w->findChild<musig::SigningSection *>();
}

// The MuSig2 rounds, open over Send.
auto signModal() -> musig::SignModal * {
    return visible<musig::SignModal>();
}

// The cosigner `fp` signing on its USB device in place: the Sign modal stays up, no
// signing window opens, its row shows On Device.
auto onDeviceShown(const QString &fp) -> bool {
    auto *m = signModal();
    if (m == nullptr || visible<SigningFlow>() != nullptr ||
        !widget()->controller()->signingKeys().contains(fp.toLower())) {
        return false;
    }
    for (auto *label : m->findChildren<QLabel *>()) {
        if (label->isVisibleTo(m) && label->text() == TR("pill-on-device")) {
            return true;
        }
    }
    return false;
}

// The Sign modal's row of the cosigner `fp`, clickable while its part is needed.
auto cosignerRow(const QString &fp) -> catalog::SelectRow * {
    auto *m = signModal();
    if (m == nullptr) {
        return nullptr;
    }
    for (auto *row : m->findChildren<catalog::SelectRow *>()) {
        if (row->isVisible() &&
            QString::compare(row->property("fingerprint").toString(), fp, Qt::CaseInsensitive) ==
                0) {
            return row;
        }
    }
    return nullptr;
}

auto receive() -> account::view::Receive * {
    auto *w = widget();
    return w == nullptr ? nullptr : w->findChild<account::view::Receive *>();
}

// The taproot address row, the only section of a musig account's Receive.
auto receiveRow() -> catalog::AddressRow * {
    return receive()->m_sections.first().row;
}

auto roundName(MusigRound r) -> const char * {
    switch (r) {
    case MusigRound::NotMusig:
        return "NotMusig";
    case MusigRound::Nonces:
        return "Nonces";
    case MusigRound::PartialSigs:
        return "PartialSigs";
    case MusigRound::Ready:
        return "Ready";
    }
    return "?";
}

auto currentRound() -> QString {
    auto *w = widget();
    if (w == nullptr) {
        return {};
    }
    auto status = w->controller()->roundStatus();
    return status.is_ok ? QString::fromLatin1(roundName(status.round)) : QString();
}

auto confirmedCoin() -> bool {
    for (const auto &coin : widget()->controller()->getCoins()) {
        if (coin.height > 0 && !coin.spent) {
            return true;
        }
    }
    return false;
}

// The leaf of Send whose signers are exactly `fps`, -1 when none.
auto leafOf(musig::SigningSection *s, QStringList fps) -> int {
    fps.sort(Qt::CaseInsensitive);
    for (std::size_t i = 0; i < s->m_leaves.leaves.size(); ++i) {
        QStringList leaf;
        for (const auto &fp : s->m_leaves.leaves[i].fingerprints) {
            leaf.append(QString::fromStdString(std::string(fp)));
        }
        leaf.sort(Qt::CaseInsensitive);
        if (QString::compare(leaf.join(','), fps.join(','), Qt::CaseInsensitive) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ===== Driver =====

struct Step {
    QString name;
    std::function<bool()> ready;
    std::function<void()> run;
};

auto always() -> std::function<bool()> {
    return [] { return true; };
}

// Runs the steps in order from its timer: a step runs once its `ready` holds, and
// fails the run when it does not within STEP_TIMEOUT_MS. Calls that open a modal
// (and so block in its event loop) are deferred, so a step returns first; the ticks
// keep running inside the modal's loop.
class Driver final : public QObject {
    Q_OBJECT

public:
    Driver() {
        m_timer.setInterval(TICK_MS);
        connect(&m_timer, &QTimer::timeout, this, &Driver::onTick, qontrol::UNIQUE);
    }

    void add(const QString &name, std::function<bool()> ready, std::function<void()> run) {
        m_steps.push_back({name, std::move(ready), std::move(run)});
    }

    // Each call gets its own single-shot timer: a timer does not fire again while its
    // own slot runs, so a call blocked in a modal never holds back the next one.
    void defer(std::function<void()> fn) {
        m_deferred.append(std::move(fn));
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, &Driver::onDeferred, qontrol::UNIQUE);
        connect(timer, &QTimer::timeout, timer, &QObject::deleteLater, qontrol::UNIQUE);
        timer->start(0);
    }

    void start() {
        m_step_timer.start();
        m_timer.start();
    }

    // Ends the run: the chain and the simulator stop, the app's loops end.
    void finish(int code) {
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_timer.stop();
        g_chain->quit(CHAIN_ANSWER_TIMEOUT_MS);
        g_coldcard.stop();
        m_exit_code = code;
        QCoreApplication::exit(code);
    }

    [[nodiscard]] auto exitCode() const -> int {
        return m_exit_code;
    }

public slots:
    void onTick() {
        if (m_running || m_finished) {
            return;
        }
        if (m_next >= m_steps.size()) {
            return;
        }
        auto &step = m_steps.at(m_next);
        if (!step.ready()) {
            if (m_step_timer.elapsed() > STEP_TIMEOUT_MS) {
                check(false, QStringLiteral("timed out at step '%1'").arg(step.name));
                m_running = true;
                shot("zz-timeout");
                m_running = false;
                std::printf("failures: %d\n", g_failures);
                finish(EXIT_TIMEOUT);
            }
            return;
        }
        std::printf("-- %s\n", step.name.toStdString().c_str());
        m_running = true;
        ++m_next;
        step.run();
        m_running = false;
        m_step_timer.restart();
    }

    // Runs the oldest deferred call; one that opens a modal returns once it closed.
    void onDeferred() {
        if (m_deferred.isEmpty() || m_finished) {
            return;
        }
        auto fn = m_deferred.takeFirst();
        fn();
    }

    void onBroadcastDone(const TxResult &result) {
        g_broadcast_answered = true;
        if (result.is_ok) {
            g_txids.append(QString::fromStdString(std::string(result.value)));
        } else {
            g_last_error = QString::fromStdString(std::string(result.error));
        }
    }

private:
    QTimer m_timer;
    std::vector<Step> m_steps;
    std::size_t m_next = 0;
    QElapsedTimer m_step_timer;
    bool m_running = false;
    bool m_finished = false;
    int m_exit_code = 0;
    QList<std::function<void()>> m_deferred;
};

Driver *g_driver = nullptr;

void later(std::function<void()> fn) {
    g_driver->defer(std::move(fn));
}

void step(const QString &name, std::function<bool()> ready, std::function<void()> run) {
    g_driver->add(name, std::move(ready), std::move(run));
}

// Picks `fp`'s row of the flow on screen and starts its route.
void pickRow(const QString &fp, const QString &what) {
    auto *flow = visible<SigningFlow>();
    int i = flowRow(flow, fp);
    bool usable = i >= 0 && flow->m_rows.at(i).usable();
    check(usable, what + " can be picked");
    if (!usable) {
        return;
    }
    later([flow, i] {
        flow->m_current = flow->m_rows.at(i);
        flow->startRoute();
    });
}

void enterPassword(keys::modal::Unseal *u) {
    u->m_password_input->setText(PASSWORD);
    later([u] { u->onNextClicked(); });
}

void checkColdcardApproved(const QString &what) {
    auto [code, output] = g_cc_driver.result();
    check(code == 0 && output.startsWith("approved"), what + ": " + output);
}

// ===== Step groups =====

// The wizard's row of `fingerprint` among `rows`.
auto wizardRow(qontrol::Column *rows, const QString &fingerprint) -> catalog::SelectRow * {
    for (auto *row : rows->findChildren<catalog::SelectRow *>()) {
        if (QString::compare(row->property("fingerprint").toString(), fingerprint,
                             Qt::CaseInsensitive) == 0) {
            return row;
        }
    }
    return nullptr;
}

auto wizardAt(CreatePolicy::View view) -> CreatePolicy * {
    auto *m = visible<CreatePolicy>();
    return m != nullptr && m->m_view == view ? m : nullptr;
}

// Registers the live device `id` as a key named `alias` from its Keys tab row.
// Starts turning the live device `id` into a key named `alias` from its Keys tab row.
void startDeviceKeySteps(const QString &id, const QString &alias, QString *fp,
                         const QString &modal_shot) {
    step("click the " + alias + " device row",
         [id] {
             auto entry = signerById(id);
             return entry.has_value() &&
                    keysTabRow("deviceInstanceId", entry->instanceId) != nullptr;
         },
         [id, fp] {
             auto entry = signerById(id);
             *fp = entry->signer->fingerprint();
             std::printf("%s fingerprint %s\n", id.toStdString().c_str(),
                         fp->toStdString().c_str());
             // Found again when it runs: the tab rebuilds its rows when the keys change.
             later([instance = entry->instanceId] {
                 if (auto *row = keysTabRow("deviceInstanceId", instance); row != nullptr) {
                     emit row->clicked();
                 }
             });
         });
    step("register " + alias, [] { return visible<keys::modal::RegisterDevice>() != nullptr; },
         [alias, modal_shot] {
             auto *m = visible<keys::modal::RegisterDevice>();
             m->m_alias->setText(alias);
             m->onAliasEdited();
             if (!modal_shot.isEmpty()) {
                 shot(modal_shot);
             }
             if (alias == QStringLiteral("Ledger")) {
                 markLedgerScreens();
             }
             later([m] { m->onRegisterClicked(); });
         });
}

// Waits for the device `id` to be the key `alias`, then checks what it holds.
void checkDeviceKeySteps(const QString &id, const QString &alias, QString *fp) {
    step(alias + " added with a USB route",
         [fp] { return visible<keys::modal::RegisterDevice>() == nullptr && hasUsbRoute(*fp); },
         [id, alias, fp] {
             check(true, alias + " is a key with a USB route");
             auto capabilities = keys::KeysModule::deviceCapabilities(signerById(id).value());
             // Both simulators sign MuSig2 (bit 3) and no USB device silent payments (bit 2).
             check(capabilities == (1U << 3),
                   alias + " reports MuSig2 and no silent payments: " +
                       QString::number(capabilities, 16));
             for (const auto &path : keys::KeysModule::deviceXpubPaths(capabilities)) {
                 check(!g_keys->cachedXpub(*fp, path).isEmpty(),
                       alias + " has its xpub at " + path);
             }
             check(g_keys->cachedXpub(*fp, QStringLiteral("m/352'/1'/0'")).isEmpty(),
                   alias + " has no silent payment xpub: it does not support them");
             if (id == LEDGER_ID) {
                 // The Ledger asks to approve the paths it does not export silently.
                 auto screens = ledgerScreensSinceMark().join('\n');
                 std::printf("Ledger screens while registering:\n  %s\n",
                             screens.toStdString().c_str());
                 check(screens.contains("87'/1'/0'") && !screens.contains("352'") &&
                           screens.contains("Approve public key"),
                       "the Ledger's 87' xpub export was approved on its screen, no 352' asked");
             }
         });
}

struct RoundSigner {
    const QString *fp;
    enum class Kind : std::uint8_t { Hot, Ledger, Coldcard } kind;
    // A shot while it waits on its password or its device, none when empty.
    QString shot;
};

// One round in the Sign modal: each cosigner clicked in order, its route approved.
// Opens the modal from Send's Sign first when it is closed.
void signRoundSteps(const QString &tag, const std::vector<RoundSigner> &signers, bool lost) {
    step(tag + ": sign modal", [] { return signModal() != nullptr || section()->m_sign_btn->isEnabled(); },
         [tag] {
             std::printf("[%s] round %s\n", tag.toStdString().c_str(),
                         currentRound().toStdString().c_str());
             if (signModal() == nullptr) {
                 later([] { section()->m_sign_btn->click(); });
             }
         });
    for (const auto &signer : signers) {
        const QString *fp = signer.fp;
        step(tag + ": click the next cosigner",
             [fp] {
                 auto *row = cosignerRow(*fp);
                 return visible<SigningFlow>() == nullptr && unsealShown() == nullptr &&
                        row != nullptr && row->isEnabled();
             },
             [fp] {
                 check(true, "signer " + *fp + " can be picked");
                 // Only the rows the list holds are drawn: none left over from an
                 // earlier render under them.
                 auto *m = signModal();
                 int drawn = 0;
                 for (auto *row : m->m_rows->findChildren<catalog::SelectRow *>(
                          Qt::FindDirectChildrenOnly)) {
                     drawn += row->isVisible() ? 1 : 0;
                 }
                 check(drawn == m->m_rows->layout()->count(),
                       QStringLiteral("the Sign modal draws its %1 rows only (%2 drawn)")
                           .arg(m->m_rows->layout()->count())
                           .arg(drawn));
                 later([fp] { emit cosignerRow(*fp)->clicked(); });
             });
        QString name = signer.shot;
        switch (signer.kind) {
        case RoundSigner::Kind::Hot:
            step(tag + ": hot password", [] { return unsealShown() != nullptr; }, [tag, name] {
                check(signModal() != nullptr,
                      tag + ": the Sign modal stays shown under the password window");
                if (!name.isEmpty()) {
                    shot(name);
                }
                enterPassword(unsealShown());
            });
            break;
        case RoundSigner::Kind::Ledger:
            step(tag + ": on the Ledger", [fp] { return onDeviceShown(*fp); }, [tag, name] {
                check(true, tag + ": the Ledger's row shows On Device in the Sign modal");
                if (!name.isEmpty()) {
                    shot(name);
                }
            });
            break;
        case RoundSigner::Kind::Coldcard:
            step(tag + ": on the Coldcard", [fp] { return onDeviceShown(*fp); }, [tag, name] {
                check(true, tag + ": the Coldcard's row shows On Device in the Sign modal");
                if (!name.isEmpty()) {
                    shot(name);
                }
                approveColdcard();
            });
            step(tag + ": Coldcard approved", [] { return g_cc_driver.finished(); },
                 [tag] { checkColdcardApproved(tag + ": Coldcard approved"); });
            break;
        }
        step(tag + ": back on the Sign modal",
             [] {
                 auto *m = signModal();
                 return visible<SigningFlow>() == nullptr && unsealShown() == nullptr &&
                        m != nullptr && widget()->controller()->signingKeys().isEmpty();
             },
             [] {});
    }
    step(tag + ": round done", always(), [tag, lost] {
        auto *m = signModal();
        bool notice = m->m_notice->isVisibleTo(m);
        auto title = m->m_notice->m_title != nullptr ? m->m_notice->m_title->text() : QString();
        auto text = m->m_notice->m_text != nullptr ? m->m_notice->m_text->text() : QString();
        std::printf("[%s] after: round %s, notice %d '%s' '%s'\n", tag.toStdString().c_str(),
                    currentRound().toStdString().c_str(), notice, title.toStdString().c_str(),
                    text.toStdString().c_str());
        if (lost) {
            check(notice && currentRound() == "Nonces" && !m->m_partial,
                  "lost session: notice shown, back to the Nonces step");
            check(title == TR("musig-sign-session-lost-title"),
                  "lost session: the session lost notice reached the Sign modal");
        } else {
            check(!notice, tag + ": no notice");
        }
        // Every nonce in: Continue moves to the partial signatures.
        if (!lost && !m->m_partial && currentRound() == "PartialSigs") {
            check(m->m_continue_btn->isEnabled(), tag + ": Continue enabled");
            later([m] { m->m_continue_btn->click(); });
        }
    });
}

// `round`: the round status once every signature is in, Ready after MuSig2 rounds,
// NotMusig for a single-key leaf.
void broadcastSteps(const QString &ready_shot, const QString &success_shot,
                    const QString &round = QStringLiteral("Ready")) {
    step("ready to broadcast",
         [] {
             auto *m = signModal();
             return m != nullptr && m->m_broadcast_btn->isVisible() &&
                    m->m_broadcast_btn->isEnabled();
         },
         [ready_shot, round] {
             check(currentRound() == round, "every signature in: " + round);
             if (!ready_shot.isEmpty()) {
                 shot(ready_shot);
             }
             g_broadcast_answered = false;
             QObject::connect(widget()->controller(), &MusigAccountController::broadcastDone,
                              g_driver, &Driver::onBroadcastDone, qontrol::UNIQUE);
             later([] { signModal()->m_broadcast_btn->click(); });
         });
    step("broadcast", [] { return g_broadcast_answered; }, [] {
        check(g_last_error.isEmpty(), "broadcast: " +
                                          (g_txids.isEmpty() ? QString() : g_txids.last()) +
                                          g_last_error);
    });
    step("success message", [] { return visible<catalog::MessageModal>() != nullptr; },
         [success_shot] {
             shot(success_shot);
             chain("mine 1");
             auto txid = g_txids.last();
             auto conf = chain("conf " + txid);
             check(conf.toInt() >= 1, "spend " + txid + " confirmations " + conf);
             later([] { visible<catalog::MessageModal>()->accept(); });
         });
}

void addSteps() {
    // ===== Devices and keys =====
    // The Keys tab polls the devices while it shows.
    step("Coldcard simulator socket", [] { return g_coldcard.socketPresent(); },
         [] { AppController::get()->showKeysTab(); });
    step("Ledger and Coldcard ready", [] {
        return signerReady(LEDGER_ID) && signerReady(COLDCARD_ID);
    }, [] {
        for (const auto &entry : Host::get()->signers()) {
            std::printf("signer %s %s instance %s fp %s state %d\n",
                        entry.signer->id().toStdString().c_str(),
                        entry.signer->brand().toStdString().c_str(),
                        entry.instanceId.toStdString().c_str(),
                        entry.signer->fingerprint().toStdString().c_str(),
                        static_cast<int>(entry.signer->state()));
        }
        shot("01-keys-devices-detected");
    });
    step("import hot key A", always(), [] {
        g_keys->importHotKey(900, g_env.seed_a, QStringLiteral("Hot"), PASSWORD);
    });
    step("hot key A imported", [] { return g_keys->tabKeys().size() == 1; }, [] {
        g_fp_a = keys::KeysModule::toQt(g_keys->tabKeys().first().fingerprint);
        std::printf("A %s\n", g_fp_a.toStdString().c_str());
    });
    // Both devices are turned into keys at once: the Coldcard starts while the Ledger
    // is still giving its xpubs.
    startDeviceKeySteps(LEDGER_ID, QStringLiteral("Ledger"), &g_fp_b,
                        QStringLiteral("02-register-ledger"));
    step("the Ledger creating its key",
         [] { return keysTab() != nullptr && keysTab()->m_creations.size() == 1; },
         [] {
             check(true, "the Ledger is being turned into a key");
             shot("02a-keys-ledger-creating");
         });
    startDeviceKeySteps(COLDCARD_ID, QStringLiteral("Coldcard"), &g_fp_c, QString());
    // The Coldcard gives its xpubs at once, the Ledger waits for its 87' approval: the
    // Coldcard is a key while the Ledger is still creating its own.
    step("the Coldcard a key while the Ledger still creates its own",
         [] { return visible<keys::modal::RegisterDevice>() == nullptr && hasUsbRoute(g_fp_c); },
         [] {
             auto ledger = signerById(LEDGER_ID);
             bool ledgerCreating =
                 ledger.has_value() && keysTab()->m_creations.contains(ledger->instanceId);
             check(ledgerCreating && !hasUsbRoute(g_fp_b),
                   "the Ledger and the Coldcard create their keys in parallel: the Coldcard "
                   "is a key while the Ledger still creates its own");
             shot("02b-keys-two-devices-creating");
         });
    checkDeviceKeySteps(LEDGER_ID, QStringLiteral("Ledger"), &g_fp_b);
    checkDeviceKeySteps(COLDCARD_ID, QStringLiteral("Coldcard"), &g_fp_c);
    step("keys tab", [] { return g_keys->tabKeys().size() == 3; }, [] {
        check(!g_fp_b.isEmpty() && !g_fp_c.isEmpty() &&
                  QString::compare(g_fp_b, g_fp_a, Qt::CaseInsensitive) != 0 &&
                  QString::compare(g_fp_c, g_fp_a, Qt::CaseInsensitive) != 0 &&
                  QString::compare(g_fp_b, g_fp_c, Qt::CaseInsensitive) != 0,
              "A, B and C are three keys: " + g_fp_a + " " + g_fp_b + " " + g_fp_c);
        shot("03-keys-a-hot-b-ledger-c-coldcard");
        g_window->m_tab->setCurrentWidget(g_window->m_menu_tab);
        Host::get()->refreshLauncher();
    });

    // ===== Creator =====
    step("launcher", always(), [] {
        shot("04-launcher");
        auto *b = launcherButton(TR("btn-create-account"), true);
        check(b != nullptr, "launcher has the create account button");
        later([b] { b->click(); });
    });
    step("account type", [] { return visible<modal::AccountType>() != nullptr; }, [] {
        auto *m = visible<modal::AccountType>();
        for (int i = 0; i < m->m_creators.size(); ++i) {
            if (m->m_creators.at(i).name == TR("musig-account-type")) {
                later([row = m->m_rows.at(i)] { emit row->clicked(); });
                return;
            }
        }
        check(false, "the account type modal lists the shared wallet");
    });
    // The first path is the default one: every key of the network, any 2 signing.
    step("spend paths", [] { return wizardAt(CreatePolicy::View::Paths) != nullptr; }, [] {
        auto *m = wizardAt(CreatePolicy::View::Paths);
        check(m->m_paths.size() == 1 && m->m_paths.first().keys.size() == 3 &&
                  m->m_paths.first().threshold == 2,
              "the default path is any 2 of the 3 keys");
        // The paths: the Ledger and the Coldcard together (the key path, a musig()
        // internal key), the hot key and the Ledger together (a MuSig2 leaf), and a
        // recovery path for the Coldcard alone, open 3 blocks after a coin confirms:
        // spend 3 signs that single-key leaf next to the musig() internal key.
        auto pair = [](const QString &first, const QString &second) {
            musig::modal::SpendPath both;
            both.keys = QStringList{first, second};
            both.threshold = 2;
            return both;
        };
        // One path only goes without a timelock: the hot key and the Ledger wait a
        // block, open by spend 2.
        auto hotLedger = pair(g_fp_a, g_fp_b);
        hotLedger.lock = musig::modal::LockKind::Relative;
        hotLedger.count = 1;
        hotLedger.unit = musig::modal::LockUnit::Blocks;
        m->m_paths = {pair(g_fp_b, g_fp_c), hotLedger};
        musig::modal::SpendPath recovery;
        recovery.name = QStringLiteral("Recovery");
        recovery.keys = QStringList{g_fp_c};
        recovery.threshold = 1;
        recovery.lock = musig::modal::LockKind::Relative;
        recovery.count = RECOVERY_BLOCKS;
        recovery.unit = musig::modal::LockUnit::Blocks;
        m->m_paths.append(recovery);
        m->showPaths();
        check(m->pathsReason().isEmpty(), "Continue enabled: " + m->pathsReason());
        shot("05-creator-paths");
        later([m] { m->onNextClicked(); });
    });
    step("fetch page", [] {
        auto *m = wizardAt(CreatePolicy::View::Xpubs);
        return m != nullptr && m->m_asks.isEmpty() && !m->missingXpubs().isEmpty();
    }, [] {
        auto *m = wizardAt(CreatePolicy::View::Xpubs);
        auto missing = m->missingXpubs();
        check(missing == QStringList{g_fp_a}, "only the hot key's xpub is missing: " +
                                                  missing.join(' '));
        auto *row = wizardRow(m->m_xpub_rows, missing.first());
        later([row] { emit row->clicked(); });
    });
    step("fetch A password", [] { return unsealShown() != nullptr; },
         [] { enterPassword(unsealShown()); });
    step("xpubs fetched", [] {
        auto *m = wizardAt(CreatePolicy::View::Xpubs);
        return m != nullptr && m->m_asks.isEmpty() && m->missingXpubs().isEmpty();
    }, [] {
        auto *m = wizardAt(CreatePolicy::View::Xpubs);
        later([m] { m->onNextClicked(); });
    });
    step("review", [] { return wizardAt(CreatePolicy::View::Review) != nullptr; }, [] {
        auto *m = wizardAt(CreatePolicy::View::Review);
        g_descriptor = m->m_descriptor;
        std::printf("descriptor %s\n", g_descriptor.toStdString().c_str());
        check(g_descriptor.contains("musig("), "musig descriptor compiled");
        for (const auto &fp : m->m_fingerprints) {
            if (fp == g_fp_b || fp == g_fp_c) {
                g_register_devices.append(fp);
            }
        }
        std::printf("registration order %s\n", m->m_fingerprints.join(' ').toStdString().c_str());
        m->m_name_input->setText(ACCOUNT);
        // The save dialog cannot be driven offscreen: the backup counts as done.
        m->m_backed_up = true;
        m->refreshReviewActions();
        shot("07-creator-review");
        check(m->m_review_next_btn->isEnabled(), "Continue enabled once named and backed up");
        later([m] { m->onNextClicked(); });
    });
    // Every device registers at once: both rows are clicked in the same pass, before
    // either answers. The Coldcard's step runs once its enroll request was sent: its
    // story waits on the device until approved.
    step("click both devices", [] {
        auto *m = wizardAt(CreatePolicy::View::Register);
        return m != nullptr && g_register_devices.size() == 2 &&
               !m->askingAny(CreatePolicy::Ask::Register);
    }, [] {
        auto *m = wizardAt(CreatePolicy::View::Register);
        QList<catalog::SelectRow *> rows;
        for (const auto &fp : g_register_devices) {
            auto *row = wizardRow(m->m_register_rows, fp);
            check(row != nullptr && row->isEnabled(), "device row to register " + fp);
            rows.append(row);
        }
        later([rows] {
            for (auto *row : rows) {
                emit row->clicked();
            }
        });
    });
    step("both devices registering at once", [] {
        if (auto *u = unsealShown(); u != nullptr) {
            enterPassword(u);
        }
        auto *m = wizardAt(CreatePolicy::View::Register);
        if (m == nullptr) {
            return false;
        }
        for (const auto &registration : g_keys->m_device_registrations) {
            if (QString::compare(registration.fingerprint, g_fp_c, Qt::CaseInsensitive) == 0 &&
                !registration.checking) {
                return true;
            }
        }
        return false;
    }, [] {
        auto *m = wizardAt(CreatePolicy::View::Register);
        check(m->asking(CreatePolicy::Ask::Register, g_fp_c),
              "the Coldcard registers, clicked while the Ledger was registering");
        check(m->asking(CreatePolicy::Ask::Register, g_fp_b) || m->m_registered.contains(g_fp_b),
              "the Ledger was asked too, not dropped for the Coldcard");
        std::printf("Ledger %s while the Coldcard enrolls\n",
                    m->m_registered.contains(g_fp_b) ? "registered" : "still registering");
        shot("08-creator-register-both");
        g_enroll_sent.start();
    });
    // The Coldcard answers its enroll request at once, then asks its user: until the
    // user approves, the wizard keeps it registering.
    step("Coldcard waits for its user", [] { return g_enroll_sent.elapsed() > ENROLL_WAIT_MS; },
         [] {
             auto *m = wizardAt(CreatePolicy::View::Register);
             check(!m->m_registered.contains(g_fp_c) &&
                       m->asking(CreatePolicy::Ask::Register, g_fp_c),
                   "the Coldcard still registering while its user has not approved");
             shot("08b-creator-register-coldcard-unconfirmed");
             approveColdcard();
         });
    step("Coldcard enrolled", [] { return g_cc_driver.finished(); },
         [] { checkColdcardApproved("the Coldcard enrolled the wallet"); });
    step("every device registered", [] {
        auto *m = wizardAt(CreatePolicy::View::Register);
        return m != nullptr && m->m_asks.isEmpty() && m->m_registered.contains(g_fp_b) &&
               m->m_registered.contains(g_fp_c) && m->m_registered.contains(g_fp_a);
    }, [] {
        auto *m = wizardAt(CreatePolicy::View::Register);
        auto hmac = Host::get()->keys()->registrationHmac(Network::Regtest, g_fp_b, g_descriptor);
        static const QRegularExpression HEX64(QStringLiteral("^[0-9a-f]{64}$"));
        check(HEX64.match(hmac).hasMatch(), "the Ledger HMAC is stored (" +
                                                QString::number(hmac.size()) + " chars)");
        check(m->m_register_next_btn->isEnabled(), "Continue enabled once every device registered");
        shot("09b-creator-registered");
        later([m] { m->onNextClicked(); });
    });
    step("verify address step", [] { return wizardAt(CreatePolicy::View::Verify) != nullptr; }, [] {
        auto *m = wizardAt(CreatePolicy::View::Verify);
        std::printf("first address %s\n", std::string(m->m_address.address).c_str());
        auto *row = wizardRow(m->m_verify_rows, g_fp_b);
        check(row != nullptr && row->isEnabled(), "check on the Ledger offered");
        markLedgerScreens();
        later([row] { emit row->clicked(); });
    });
    // A click sends the address straight to the device: no signer picker, no window.
    step("verify on the Ledger", [] {
        auto *m = wizardAt(CreatePolicy::View::Verify);
        return m != nullptr && m->asking(CreatePolicy::Ask::Verify, g_fp_b);
    }, [] {
        check(visible<SigningFlow>() == nullptr, "no signer picker opened over the Verify step");
        shot("10-creator-verify-ledger");
    });
    step("verified", [] {
        auto *m = wizardAt(CreatePolicy::View::Verify);
        return m != nullptr && m->m_asks.isEmpty();
    }, [] {
        auto *m = wizardAt(CreatePolicy::View::Verify);
        check(m->m_verified.contains(g_fp_b), "the Ledger row reads Verified");
        auto address = QString::fromStdString(std::string(m->m_address.address));
        // The Bitcoin Test app writes a regtest address with the testnet prefix: the
        // part between the prefix and the checksum is the same.
        auto program = address.section('1', 1).chopped(BECH32_CHECKSUM_LENGTH);
        check(!address.isEmpty() && !program.isEmpty() &&
                  ledgerScreensText().contains("tb1" + program),
              "the Ledger screens showed " + address + " (tb1" + program + "...)");
        shot("11-creator-verify-matched");
        later([m] { m->onNextClicked(); });
    });
    step("electrum", [] { return wizardAt(CreatePolicy::View::Electrum) != nullptr; }, [] {
        auto *m = wizardAt(CreatePolicy::View::Electrum);
        m->m_electrum.input()->setText(g_electrum);
        m->onElectrumEdited();
        later([m] { m->onTestElectrum(); });
    });
    step("electrum tested", [] {
        auto *m = wizardAt(CreatePolicy::View::Electrum);
        return m != nullptr && (m->m_electrum.state() == catalog::EndpointRow::State::Ok ||
                                m->m_electrum.state() == catalog::EndpointRow::State::Failed);
    }, [] {
        auto *m = wizardAt(CreatePolicy::View::Electrum);
        check(m->m_electrum.state() == catalog::EndpointRow::State::Ok,
              "electrum test passes against electrs");
        shot("11b-creator-electrum");
        g_instances_before = Host::get()->instances().size();
        later([m] { m->m_install_btn->click(); });
    });
    step("launcher lists it", [] {
        return visible<CreatePolicy>() == nullptr && launcherButton(ACCOUNT, true) != nullptr;
    }, [] {
        check(Host::get()->instances().size() == g_instances_before, "installed, not opened");
        shot("12-launcher-after-install");
        auto *b = launcherButton(ACCOUNT, true);
        later([b] { b->click(); });
    });

    // ===== Receive =====
    step("account open", [] { return widget() != nullptr; },
         [] { widget()->controller()->receiveClicked(); });
    step("receive shown", [] { return receive() != nullptr; },
         [] { later([] { emit receiveRow()->generateRequested(); }); });
    step("address generated", [] { return !receiveRow()->address().isEmpty(); }, [] {
        std::printf("receive %s\n", receiveRow()->address().toStdString().c_str());
        shot("13-receive-generated");
        later([] { emit receiveRow()->verifyRequested(); });
    });
    step("receive verify picker", [] { return visible<SigningFlow>() != nullptr; },
         [] { pickRow(g_fp_c, "the Coldcard, to show the address"); });
    // The Coldcard answers with the address at once and keeps it on screen in a story.
    step("receive on the Coldcard", [] {
        auto *f = visible<SigningFlow>();
        return usbWaiting(g_fp_c) || (f != nullptr && f->m_verify_result.has_value());
    }, [] { approveColdcard(); });
    step("Coldcard showed the address", [] { return g_cc_driver.finished(); },
         [] { checkColdcardApproved("the Coldcard showed the receive address"); });
    step("receive verified", [] {
        auto *f = visible<SigningFlow>();
        return f != nullptr && f->m_verify_result.has_value();
    }, [] {
        auto *f = visible<SigningFlow>();
        check(f->m_verify_result.value() == SigningFlow::VerifyResult::Matched,
              "the Coldcard matched the receive address");
        dismissColdcard();
    });
    step("Coldcard story dismissed", [] { return g_cc_driver.finished(); }, [] {
        g_cc_driver.result();
        shot("14-receive-verified-coldcard");
        later([] { visible<SigningFlow>()->accept(); });
    });
    // The same address on the Ledger, from Receive's picker.
    step("receive verify on the Ledger", [] { return visible<SigningFlow>() == nullptr; }, [] {
        markLedgerScreens();
        later([] { emit receiveRow()->verifyRequested(); });
    });
    step("receive verify picker 2", [] { return visible<SigningFlow>() != nullptr; },
         [] { pickRow(g_fp_b, "the Ledger, to show the address"); });
    step("receive verified on the Ledger", [] {
        auto *f = visible<SigningFlow>();
        return f != nullptr && f->m_verify_result.has_value();
    }, [] {
        auto *f = visible<SigningFlow>();
        check(f->m_verify_result.value() == SigningFlow::VerifyResult::Matched,
              "the Ledger matched the receive address");
        shot("14b-receive-verified-ledger");
        later([f] { f->accept(); });
    });
    step("fund", [] { return visible<SigningFlow>() == nullptr; }, [] {
        auto address = receiveRow()->address();
        check(!chain("send " + address + " 0.02").isEmpty(), "funded");
        chain("mine 1");
    });
    step("coin confirmed", confirmedCoin, [] { widget()->controller()->sendClicked(); });

    // ===== Spend 1: the Ledger + Coldcard leaf =====
    step("send shown", [] { return sendScreen() != nullptr && section() != nullptr; }, [] {
        auto *payment = sendScreen()->m_outputs.begin().value();
        payment->m_address_input->setText(chain("newaddr"));
        payment->m_amount_input->setText("0.005");
    });
    step("leaf picker", [] { return section()->m_leaf_combo->count() > 0; }, [] {
        auto *s = section();
        int bc = leafOf(s, {g_fp_b, g_fp_c});
        check(bc >= 0, "a Ledger + Coldcard leaf");
        s->m_leaf_combo->setCurrentIndex(bc);
        sendScreen()->onProcess();
        later([s] { s->m_leaf_combo->showPopup(); });
    });
    step("leaf popup", always(), [] {
        shot("15-send-leaf-picker");
        section()->m_leaf_combo->hidePopup();
    });
    step("sign", [] { return section()->m_sign_btn->isEnabled(); },
         [] { later([] { section()->m_sign_btn->click(); }); });
    step("prepared", [] { return signModal() != nullptr; }, [] {
        check(currentRound() == "Nonces", "prepared: round 1");
        shot("16-sign-modal-round1");
    });
    using Kind = RoundSigner::Kind;
    // Round 1 on both devices at once: both rows clicked in the same pass, the
    // Coldcard asked while the Ledger works on its nonce.
    step("round 1: both devices clicked", [] {
        auto *b = cosignerRow(g_fp_b);
        auto *c = cosignerRow(g_fp_c);
        return b != nullptr && c != nullptr && b->isEnabled() && c->isEnabled();
    }, [] {
        auto *b = cosignerRow(g_fp_b);
        auto *c = cosignerRow(g_fp_c);
        later([b, c] {
            emit b->clicked();
            emit c->clicked();
        });
    });
    step("round 1: both on their device",
         [] { return onDeviceShown(g_fp_b) && onDeviceShown(g_fp_c); }, [] {
             check(true, "round 1: the Ledger and the Coldcard both On Device at once");
             shot("17-round1-both-devices");
             approveColdcard();
         });
    step("round 1: Coldcard approved", [] { return g_cc_driver.finished(); },
         [] { checkColdcardApproved("round 1: Coldcard approved"); });
    step("round 1: both answered", [] {
        return signModal() != nullptr && widget()->controller()->signingKeys().isEmpty();
    }, [] {
        auto *m = signModal();
        bool notice = m->m_notice->isVisibleTo(m);
        auto text = m->m_notice->m_text != nullptr ? m->m_notice->m_text->text() : QString();
        check(!notice, "round 1: both nonces merged, no notice" + (notice ? ": " + text : QString()));
        check(currentRound() == "PartialSigs", "round 1: every nonce in: " + currentRound());
        shot("18-round1-both-nonces");
        later([m] { m->m_continue_btn->click(); });
    });
    step("after round 1", [] { return signModal() != nullptr && signModal()->m_partial; }, [] {
        check(currentRound() == "PartialSigs", "round 1 done: round 2 next");
        shot("19-after-round1");
        // Closed, the modal keeps the session: Sign opens it again at its round.
        later([] { signModal()->reject(); });
    });
    // phase 29's lost session: the simulator restarts with the same seed and nothing
    // enrolled (--eff). Receive's verify picker stays open meanwhile: its Coldcard row
    // follows the device, gone then back.
    step("verify picker open", [] { return signModal() == nullptr; }, [] {
        widget()->controller()->receiveClicked();
        later([] { emit receiveRow()->verifyRequested(); });
    });
    step("verify picker lists the Coldcard",
         [] { return pickerStatus(g_fp_c) == keys::flow::SignerStatus::Ready; }, [] {
             check(true, "the verify picker lists the Coldcard ready");
             g_coldcard.stop();
         });
    step("Coldcard dropped",
         [] {
             return !signerById(COLDCARD_ID).has_value() &&
                    pickerStatus(g_fp_c) == keys::flow::SignerStatus::Disconnected;
         },
         [] {
             check(true, "the hwi service dropped the Coldcard, its picker row Not Connected");
             shot("19b-verify-picker-coldcard-gone");
             g_coldcard.start();
         });
    step("picker sees the Coldcard back",
         [] { return pickerStatus(g_fp_c) == keys::flow::SignerStatus::Ready; }, [] {
             check(true, "the restarted Coldcard is ready again in the picker");
             shot("19c-verify-picker-coldcard-back");
             later([] {
                 visible<SigningFlow>()->reject();
                 widget()->controller()->sendClicked();
             });
         });
    step("Coldcard reacquired", [] { return signerReady(COLDCARD_ID); }, [] {
        auto fp = signerById(COLDCARD_ID)->signer->fingerprint();
        check(QString::compare(fp, g_fp_c, Qt::CaseInsensitive) == 0,
              "the restarted Coldcard is key C again: " + fp);
        g_cc_driver.start({"story"});
    });
    step("restarted Coldcard story", [] { return g_cc_driver.finished(); }, [] {
        auto [code, output] = g_cc_driver.result();
        check(code == 0 && output.isEmpty(), "the restarted Coldcard shows no story");
        AppController::get()->showKeysTab();
    });
    step("open key C", [] { return keysTabRow("keyFingerprint", g_fp_c) != nullptr; }, [] {
        later([] {
            if (auto *row = keysTabRow("keyFingerprint", g_fp_c); row != nullptr) {
                emit row->clicked();
            }
        });
    });
    step("key C checked on the Coldcard", [] {
        auto *d = visible<keys::modal::KeyDetail>();
        return d != nullptr && d->m_registrations.contains(g_descriptor) &&
               d->m_registrations.value(g_descriptor) == keys::modal::Registration::NotRegistered;
    }, [] {
        auto *d = visible<keys::modal::KeyDetail>();
        QPushButton *button = nullptr;
        for (auto *b : d->findChildren<QPushButton *>()) {
            if (b->property("descriptor").toString() == g_descriptor && b->isVisible()) {
                button = b;
            }
        }
        check(button != nullptr && button->isEnabled(),
              "key C's detail offers to register the account on the Coldcard");
        if (button != nullptr) {
            later([button] { button->click(); });
        }
    });
    step("enroll again on the Coldcard", [] {
        auto *d = visible<keys::modal::KeyDetail>();
        if (d == nullptr) {
            return false;
        }
        auto state = d->m_registrations.value(g_descriptor);
        return (state == keys::modal::Registration::Registering && d->m_register_req != 0) ||
               state == keys::modal::Registration::Registered;
    }, [] { approveColdcard(); });
    step("Coldcard enrolled again", [] { return g_cc_driver.finished(); },
         [] { checkColdcardApproved("the restarted Coldcard enrolled the wallet"); });
    step("key C registered", [] {
        auto *d = visible<keys::modal::KeyDetail>();
        return d != nullptr &&
               d->m_registrations.value(g_descriptor) == keys::modal::Registration::Registered;
    }, [] {
        // Registered once the Coldcard lists the wallet, after its approval.
        shot("20-reenroll-coldcard");
        auto *d = visible<keys::modal::KeyDetail>();
        later([d] { d->accept(); });
    });
    step("back to the account", [] { return visible<keys::modal::KeyDetail>() == nullptr; }, [] {
        for (const auto &tab : g_window->m_tabs) {
            if (tab.second == widget() || tab.second->isAncestorOf(widget())) {
                g_window->m_tab->setCurrentWidget(tab.second);
            }
        }
    });
    signRoundSteps("round 2 lost",
                   {{&g_fp_b, Kind::Ledger, QString()}, {&g_fp_c, Kind::Coldcard, QString()}},
                   true);
    step("lost session shot", always(), [] {
        shot("21-lost-session-notice");
        dismissColdcard();
    });
    step("lost session story dismissed", [] { return g_cc_driver.finished(); },
         [] { g_cc_driver.result(); });
    signRoundSteps("round 1 again",
                   {{&g_fp_b, Kind::Ledger, "22-round1-again-ledger-usb"},
                    {&g_fp_c, Kind::Coldcard, "22-round1-again-coldcard-usb"}},
                   false);
    step("round 1 again done", always(), [] {
        check(currentRound() == "PartialSigs", "round 1 again done: round 2 next");
    });
    signRoundSteps("round 2",
                   {{&g_fp_b, Kind::Ledger, "23-round2-ledger-usb"},
                    {&g_fp_c, Kind::Coldcard, "24-round2-coldcard-usb"}},
                   false);
    broadcastSteps("25-ready-to-broadcast", "26-broadcast-success");

    // ===== Spend 2: the Ledger + hot leaf =====
    step("change confirmed",
         [] { return visible<catalog::Modal>() == nullptr && confirmedCoin(); },
         [] { widget()->controller()->sendClicked(); });
    step("send again", [] { return section() != nullptr && !section()->m_session; }, [] {
             auto *payment = sendScreen()->m_outputs.begin().value();
             payment->m_address_input->setText(chain("newaddr"));
             payment->m_amount_input->setText("0.004");
         });
    step("hot leaf", [] { return section()->m_leaf_combo->count() > 0; }, [] {
        auto *s = section();
        int ab = leafOf(s, {g_fp_a, g_fp_b});
        check(ab >= 0, "a Ledger + hot leaf");
        s->m_leaf_combo->setCurrentIndex(ab);
        sendScreen()->onProcess();
    });
    step("sign 2", [] { return section()->m_sign_btn->isEnabled(); }, [] {
        shot("27-send-ledger-hot-leaf");
        later([] { section()->m_sign_btn->click(); });
    });
    step("prepared 2", [] { return signModal() != nullptr; },
         [] { check(currentRound() == "Nonces", "prepared 2: round 1"); });
    signRoundSteps("spend 2 round 1",
                   {{&g_fp_a, Kind::Hot, "28-round1-hot-password"},
                    {&g_fp_b, Kind::Ledger, QString()}},
                   false);
    step("spend 2 round 1 done", always(), [] {
        check(currentRound() == "PartialSigs", "spend 2 round 1 done: round 2 next");
    });
    signRoundSteps("spend 2 round 2",
                   {{&g_fp_a, Kind::Hot, QString()}, {&g_fp_b, Kind::Ledger, "29-round2-ledger-usb"}},
                   false);
    broadcastSteps(QString(), "30-broadcast-success-2");

    // ===== Spend 3: the Coldcard alone, on its timelocked leaf =====
    step("mine past the recovery timelock", [] { return visible<catalog::Modal>() == nullptr; },
         [] {
             chain(QStringLiteral("mine %1").arg(RECOVERY_BLOCKS + 1));
             widget()->controller()->sendClicked();
         });
    step("send 3", [] { return section() != nullptr && !section()->m_session; }, [] {
        auto *payment = sendScreen()->m_outputs.begin().value();
        payment->m_address_input->setText(chain("newaddr"));
        payment->m_amount_input->setText("0.002");
    });
    step("Coldcard leaf open", [] {
        auto *s = section();
        int c = leafOf(s, {g_fp_c});
        return c >= 0 && s->m_leaves.leaves[c].spendable;
    }, [] {
        auto *s = section();
        int c = leafOf(s, {g_fp_c});
        check(true, "the Coldcard's recovery leaf is spendable");
        s->m_leaf_combo->setCurrentIndex(c);
        sendScreen()->onProcess();
    });
    step("sign 3", [] { return section()->m_sign_btn->isEnabled(); }, [] {
        shot("31-send-coldcard-leaf");
        later([] { section()->m_sign_btn->click(); });
    });
    step("prepared 3", [] { return signModal() != nullptr; }, [] {
        auto *m = signModal();
        check(currentRound() == "NotMusig", "prepared 3: a single-key leaf, no MuSig2 round");
        // The path's signer first, the wallet's other keys after it, greyed.
        QStringList order;
        for (int i = 0; i < m->m_rows->layout()->count(); ++i) {
            auto *row = qobject_cast<catalog::SelectRow *>(m->m_rows->layout()->itemAt(i)->widget());
            if (row != nullptr) {
                order.append(row->property("fingerprint").toString() +
                             (row->isEnabled() ? "" : " (greyed)"));
            }
        }
        check(order.size() == 3 && order.first() == g_fp_c && order.at(1).endsWith("(greyed)") &&
                  order.at(2).endsWith("(greyed)"),
              "every key listed, the Coldcard first, the others greyed: " + order.join(", "));
        shot("32-sign-coldcard-leaf");
        later([] { emit cosignerRow(g_fp_c)->clicked(); });
    });
    step("spend 3: on the Coldcard", [] { return onDeviceShown(g_fp_c); }, [] {
        shot("33-coldcard-leaf-on-device");
        approveColdcard();
    });
    step("spend 3: Coldcard approved", [] { return g_cc_driver.finished(); },
         [] { checkColdcardApproved("spend 3: Coldcard approved"); });
    step("spend 3: signed", [] {
        auto *m = signModal();
        return m != nullptr && widget()->controller()->signingKeys().isEmpty();
    }, [] {
        auto *m = signModal();
        bool notice = m->m_notice->isVisibleTo(m);
        auto text = m->m_notice->m_text != nullptr ? m->m_notice->m_text->text() : QString();
        check(!notice, "spend 3: the Coldcard signed its single-key leaf" +
                           (notice ? ": " + text : QString()));
    });
    broadcastSteps("34-coldcard-leaf-ready", "35-broadcast-success-3", QStringLiteral("NotMusig"));

    // ===== Settings: Register Descriptor =====
    // Every device already holds the account: asked on open, each row reads Registered,
    // the hot key Not Needed; nothing is registered again.
    step("settings", [] { return visible<catalog::Modal>() == nullptr; },
         [] { widget()->controller()->settingsClicked(); });
    step("electrum height", [] {
        auto *settings = widget()->findChild<account::view::Settings *>();
        bool ok = false;
        return settings != nullptr && settings->isVisible() &&
               (settings->m_electrum_height_label->text().toUInt(&ok), ok);
    }, [] {
        auto *settings = widget()->findChild<account::view::Settings *>();
        auto shown = settings->m_electrum_height_label->text();
        auto tip = chain("mine 0");
        check(shown == tip, "Settings show the Electrum block height " + shown + " (chain " + tip + ")");
    });
    step("register descriptor button", [] {
        auto *extras = widget()->findChild<musig::SettingsExtras *>();
        return extras != nullptr && extras->m_register_btn->isVisible();
    }, [] {
        shot("36a-settings-register-descriptor");
        auto *extras = widget()->findChild<musig::SettingsExtras *>();
        later([extras] { extras->m_register_btn->click(); });
    });
    step("register descriptor checked", [] {
        auto *m = visible<musig::modal::RegisterDescriptor>();
        return m != nullptr && m->m_requests.isEmpty() &&
               m->m_states.value(g_fp_b) != musig::modal::RegisterDescriptor::State::Checking;
    }, [] {
        auto *m = visible<musig::modal::RegisterDescriptor>();
        using State = musig::modal::RegisterDescriptor::State;
        check(m->m_states.value(g_fp_b) == State::Registered,
              "Register Descriptor: the Ledger holds the account");
        check(m->m_states.value(g_fp_c) == State::Registered,
              "Register Descriptor: the Coldcard holds the account");
        check(m->m_states.value(g_fp_a) == State::NotNeeded,
              "Register Descriptor: the hot key needs no registration");
        shot("36b-register-descriptor");
        later([m] { m->accept(); });
    });

    // ===== Electrum going away =====
    // The server closes the connection (a restart): the account sees it and reconnects.
    step("electrum connected", [] { return electrumUp(); }, [] {
        check(chain("relay drop").toInt() >= 1, "the relay dropped the account's connection");
        g_since.start();
    });
    step("electrum drop seen", [] { return !electrumUp(); }, [] {
        std::printf("drop seen after %lld ms\n", g_since.elapsed());
        g_since.start();
    });
    step("electrum reconnected", [] { return electrumUp(); }, [] {
        check(true, QStringLiteral("the account reconnected after the server closed, in %1 ms")
                        .arg(g_since.elapsed()));
        // A link dead with no close (sleep, NAT, wifi lost): only the keepalive's
        // unanswered ping can tell.
        check(chain("relay mute") == "muted", "the relay went silent");
        g_since.start();
    });
    step("electrum silent death seen", [] { return !electrumUp(); }, [] {
        check(g_since.elapsed() < KEEPALIVE_DETECT_MS,
              QStringLiteral("the dead link was caught in %1 ms").arg(g_since.elapsed()));
        check(chain("relay unmute") == "unmuted", "the relay relays again");
        g_since.start();
    });
    step("electrum reconnected after the dead link", [] { return electrumUp(); }, [] {
        check(true, QStringLiteral("the account reconnected after the dead link, in %1 ms")
                        .arg(g_since.elapsed()));
    });

    // ===== End =====
    step("history", [] { return visible<catalog::Modal>() == nullptr; },
         [] { widget()->controller()->historyClicked(); });
    step("end", always(), [] {
        shot("37-history-three-spends");
        check(g_txids.size() == 3, "three spends broadcast");
        for (const auto &txid : g_txids) {
            std::printf("txid %s\n", txid.toStdString().c_str());
        }
        std::printf("shots: %d\nfailures: %d\n", g_shot_count, g_failures);
        g_driver->finish(g_failures == 0 ? 0 : 1);
    });
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_env = Env{
        .run_dir = requireEnv("E2E_RUN_DIR"),
        .chain_bin = requireEnv("E2E_CHAIN_BIN"),
        .coldcard_unix_dir = requireEnv("E2E_COLDCARD_UNIX_DIR"),
        .python = requireEnv("E2E_PYTHON"),
        .drivers_dir = requireEnv("E2E_DRIVERS_DIR"),
        .ledger_screens = requireEnv("E2E_LEDGER_SCREENS"),
        .seed_a = requireEnv("E2E_SEED_A"),
        .seed_c = requireEnv("E2E_SEED_C"),
        .coldcard_socket = requireEnv("BWK_COLDCARD_SIMULATOR"),
    };
    QDir().mkpath(g_env.run_dir + "/shots");

    QApplication app(argc, argv);
    QFontDatabase::addApplicationFontFromData(QByteArray(
        reinterpret_cast<const char *>(embedded_font::NOTO_SANS), embedded_font::NOTO_SANS_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_MONO),
                   embedded_font::NOTO_SANS_MONO_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_BOLD),
                   embedded_font::NOTO_SANS_BOLD_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_MONO_BOLD),
                   embedded_font::NOTO_SANS_MONO_BOLD_SIZE));
    qRegisterMetaType<BackendInfo>();
    qRegisterMetaType<ConnectionResult>();
    qRegisterMetaType<TxResult>();
    qRegisterMetaType<Notification>();
    qRegisterMetaType<HwiEvent>();
    qRegisterMetaType<HwiDevice>();
    qRegisterMetaType<qr::QrRender>();
    qRegisterMetaType<qr::QrScan>();
    qRegisterMetaType<MnemonicResult>();
    qRegisterMetaType<KeyInfoResult>();
    qRegisterMetaType<std::optional<ReqId>>();
    qRegisterMetaType<plugin::SignError>();
    init_logging(LogLevel::Error);

    // The chain first: electrs is up before the creator tests it.
    g_chain = new LineProcess(g_env.chain_bin, g_env.run_dir + "/chain.stderr.log");
    auto ready = g_chain->start(CHAIN_START_TIMEOUT_MS);
    check(ready.startsWith("READY "), "chain up: " + ready);
    if (!ready.startsWith("READY ")) {
        return 1;
    }
    // Through the chain's relay: the run can make the server go away under the app.
    g_electrum = "tcp://127.0.0.1:" + chain("relay");
    std::printf("electrum %s (relaying %s)\n", g_electrum.toStdString().c_str(),
                ready.toStdString().c_str());
    g_coldcard.start();

    Theme::init();
    Theme::get()->apply();
    i18n::I18nManager::get()->init(&app);
    AppController::init();
    auto *controller = AppController::get();
    Host::get();
    controller->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    controller->registerBuiltinPlugin(std::make_unique<MusigPlugin>());
    controller->registerBuiltinPlugin(std::make_unique<HwiPlugin>());
    auto keysModule = std::make_unique<keys::KeysModule>();
    g_keys = keysModule.get();
    controller->registerBuiltinPlugin(std::move(keysModule));
    g_window = new MainWindow();
    controller->start(g_window);
    controller->initState();
    g_window->move(0, 0);
    g_window->resize(WINDOW_WIDTH, WINDOW_HEIGHT);
    g_window->show();
    g_window->activateWindow();
    spin(1500);
    // Regtest through the Keys tab's own network selector, as a user picks it.
    auto *tab = keysTab();
    check(tab != nullptr, "keys tab");
    int regtest = tab->m_network_selector->findData(static_cast<int>(Network::Regtest));
    tab->m_network_selector->setCurrentIndex(regtest);
    tab->onNetworkChanged();

    g_driver = new Driver;
    addSteps();
    g_driver->start();
    QApplication::exec();
    int code = g_driver->exitCode();
    // The app's teardown waits on device and scanner threads the run does not need
    // stopped cleanly: the result is final here.
    std::fflush(stdout);
    std::_Exit(code);
}

#include "main.moc"
