// Device GUI end to end (tests/e2e/devices/run.sh): the real silent app on a private
// Xvfb display (QT_QPA_PLATFORM=xcb) against a regtest chain (the e2e_chain example),
// a Ledger on Speculos (key B) and the Coldcard simulator (key C), with key A hot in
// silent. A 2-of-3 MuSig2 shared wallet is created, registered on both devices,
// funded, and spent twice: through the Ledger + Coldcard leaf (with a lost session,
// the Coldcard simulator restarted between round 1 and round 2) and through the
// Ledger + hot leaf. The Ledger is approved by ledger_clicker.py through the Speculos
// REST API, the Coldcard by coldcard_driver.py through the simulator's key input.
// Every shot is the window area grabbed from the screen, any modal over it included.
#include <QtWidgets>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#define private public
#define protected public
#include "AppController.h"
#include "MainWindow.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "account/views/Settings.h"
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
#include "host/Host.h"
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
#include "modules/musig/RegisterDescriptor.h"
#include "modules/musig/SettingsExtras.h"
#include "modules/musig/SignModal.h"
#include "modules/musig/SigningSection.h"
#include "modules/musig/creator/CreatePolicy.h"
#include "modules/musig/creator/MusigCreatorModule.h"
#include "modules/sp/SpPlugin.h"
#include "qr/QrTypes.h"
#include "resources/font/noto_sans.h"
#include "resources/font/noto_sans_bold.h"
#include "resources/font/noto_sans_mono.h"
#include "resources/font/noto_sans_mono_bold.h"
#include "state/DeviceUnlock.h"
#include "state/StateSetup.h"
#include "state/UnlockDevices.h"
#include "state/statesetup/DeviceFlow.h"
#include "theme/Theme.h"
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

Env gEnv;
MainWindow *gWindow = nullptr;
keys::KeysModule *gKeys = nullptr;
int gFailures = 0;
QString gFpA;
QString gFpB;
QString gFpC;
QString gElectrum;
QString gDescriptor;
QStringList gTxids;
// B and C in the order the creator registers them.
QStringList gRegisterDevices;
QString gLastError;
bool gBroadcastAnswered = false;
qsizetype gInstancesBefore = 0;
// Where the Ledger screens log stood when a check on it started.
qint64 gScreensMark = 0;

void check(bool ok, const QString &what) {
    std::printf("%s: %s\n", ok ? "OK  " : "FAIL", what.toStdString().c_str());
    if (!ok) {
        ++gFailures;
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

LineProcess *gChain = nullptr;

auto chain(const QString &command) -> QString {
    auto answer = gChain->ask(command, CHAIN_ANSWER_TIMEOUT_MS);
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
        m_process->setProgram(gEnv.python);
        m_process->setArguments({"simulator.py", "--eff", "--headless", "--seed", gEnv.seed_c});
        m_process->setWorkingDirectory(gEnv.coldcard_unix_dir);
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("SDL_VIDEODRIVER", "dummy");
        m_process->setProcessEnvironment(env);
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        m_process->setStandardInputFile(QProcess::nullDevice());
        m_process->setStandardOutputFile(gEnv.run_dir + "/coldcard-sim.log", QIODevice::Append);
        m_process->setChildProcessModifier(ownProcessGroup);
        m_process->start();
        check(m_process->waitForStarted(), "Coldcard simulator started");
        m_pid = m_process->processId();
        QFile pidFile(gEnv.run_dir + "/coldcard-sim.pid");
        if (pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pidFile.write(QByteArray::number(m_pid) + "\n");
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
        QFile::remove(gEnv.coldcard_socket);
        QDir(QStringLiteral("/tmp/cc-simulators/%1").arg(m_pid)).removeRecursively();
        QFile::remove(gEnv.run_dir + "/coldcard-sim.pid");
        m_process = nullptr;
    }

    [[nodiscard]] static auto socketPresent() -> bool {
        return QFileInfo(gEnv.coldcard_socket).exists();
    }

private:
    std::unique_ptr<QProcess> m_process;
    qint64 m_pid = 0;
};

ColdcardSim gColdcard;

// One coldcard_driver.py command at a time, run without blocking the event loop and
// polled by the steps.
class ColdcardDriver {
public:
    void start(const QStringList &args) {
        m_process = std::make_unique<QProcess>();
        m_process->setProgram(gEnv.python);
        m_process->setArguments(QStringList{"-I", gEnv.drivers_dir + "/coldcard_driver.py",
                                            "--socket", gEnv.coldcard_socket} +
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

ColdcardDriver gCcDriver;
// Since the last Electrum event the run caused.
QElapsedTimer gSince;
// A ping every 20 s, unanswered after 5 s: a dead link is caught within this.
constexpr qint64 KEEPALIVE_DETECT_MS = 40000;
// The relative timelock, in blocks, of the Coldcard's recovery path.
constexpr int RECOVERY_BLOCKS = 3;
// Since the wizard sent the Coldcard its enroll request.
QElapsedTimer gEnrollSent;
// How long the Coldcard is left unapproved before the check.
constexpr qint64 ENROLL_WAIT_MS = 3000;

void approveColdcard() {
    gCcDriver.start({"approve", "--timeout", QString::number(APPROVE_TIMEOUT_S)});
}

void dismissColdcard() {
    gCcDriver.start({"dismiss"});
}

// ===== Ledger screens =====

// The screens ledger_clicker.py logged since the mark, one per line, timestamp dropped.
auto ledgerScreensSinceMark() -> QStringList {
    QFile file(gEnv.ledger_screens);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    file.seek(gScreensMark);
    QStringList screens;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts)) {
        screens.append(line.section(' ', 1));
    }
    return screens;
}

void markLedgerScreens() {
    gScreensMark = QFileInfo(gEnv.ledger_screens).size();
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

int gShotCount = 0;
// The window shot before the main window exists: the state's setup and unlock.
QWidget *gFirstWindow = nullptr;

void shot(const QString &name) {
    spin(SHOT_SETTLE_MS);
    auto g = (gWindow != nullptr ? static_cast<QWidget *>(gWindow) : gFirstWindow)->frameGeometry();
    auto pixmap =
        QGuiApplication::primaryScreen()->grabWindow(0, g.x(), g.y(), g.width(), g.height());
    QString path = gEnv.run_dir + "/shots/" + name + ".png";
    bool saved = pixmap.save(path);
    ++gShotCount;
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
    for (auto *button : gWindow->findChildren<QPushButton *>()) {
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
    for (const auto &info : gKeys->tabKeys()) {
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
    return std::ranges::any_of(info->routes,
                               [](KeyRouteKind route) { return route == KeyRouteKind::Usb; });
}

auto keysTab() -> keys::KeysTab * {
    return gWindow->findChild<keys::KeysTab *>();
}

// The Keys tab row of a live device, or of a key.
auto keysTabRow(const char *property, const QString &value) -> catalog::SelectRow * {
    auto *tab = keysTab();
    if (tab == nullptr) {
        return nullptr;
    }
    for (auto *row : tab->findChildren<catalog::SelectRow *>()) {
        if (QString::compare(row->property(property).toString(), value, Qt::CaseInsensitive) == 0) {
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
    return gWindow->findChild<MusigAccountWidget *>();
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
        if (row->isVisible() && QString::compare(row->property("fingerprint").toString(), fp,
                                                 Qt::CaseInsensitive) == 0) {
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
    return std::ranges::any_of(widget()->controller()->getCoins(),
                               [](const auto &coin) { return coin.height > 0 && !coin.spent; });
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
        gChain->quit(CHAIN_ANSWER_TIMEOUT_MS);
        gColdcard.stop();
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
                std::printf("failures: %d\n", gFailures);
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

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void onBroadcastDone(const TxResult &result) {
        gBroadcastAnswered = true;
        if (result.is_ok) {
            gTxids.append(QString::fromStdString(std::string(result.value)));
        } else {
            gLastError = QString::fromStdString(std::string(result.error));
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

Driver *gDriver = nullptr;

void later(std::function<void()> fn) {
    gDriver->defer(std::move(fn));
}

void step(const QString &name, std::function<bool()> ready, std::function<void()> run) {
    gDriver->add(name, std::move(ready), std::move(run));
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
    auto [code, output] = gCcDriver.result();
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
    step(
        "click the " + alias + " device row",
        [id] {
            auto entry = signerById(id);
            return entry.has_value() &&
                   keysTabRow("deviceInstanceId", entry->instanceId) != nullptr;
        },
        [id, fp] {
            auto entry = signerById(id);
            *fp = entry->signer->fingerprint();
            std::printf("%s fingerprint %s\n", id.toStdString().c_str(), fp->toStdString().c_str());
            // Found again when it runs: the tab rebuilds its rows when the keys change.
            later([instance = entry->instanceId] {
                if (auto *row = keysTabRow("deviceInstanceId", instance); row != nullptr) {
                    emit row->clicked();
                }
            });
        });
    step(
        "register " + alias, [] { return visible<keys::modal::RegisterDevice>() != nullptr; },
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
    step(
        alias + " added with a USB route",
        [fp] { return visible<keys::modal::RegisterDevice>() == nullptr && hasUsbRoute(*fp); },
        [id, alias, fp] {
            check(true, alias + " is a key with a USB route");
            auto capabilities = keys::KeysModule::deviceCapabilities(signerById(id).value());
            // Both simulators support ordinary/Taproot/MuSig2, but no silent payments.
            check(capabilities == 11U,
                  alias + " reports ordinary/Taproot/MuSig2 and no silent payments: " +
                      QString::number(capabilities, 16));
            for (const auto &path : keys::KeysModule::deviceXpubPaths(capabilities)) {
                check(!gKeys->cachedXpub(*fp, path).isEmpty(), alias + " has its xpub at " + path);
            }
            check(gKeys->cachedXpub(*fp, QStringLiteral("m/352'/1'/0'")).isEmpty(),
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
    step(
        tag + ": sign modal",
        [] { return signModal() != nullptr || section()->m_sign_btn->isEnabled(); },
        [tag] {
            std::printf("[%s] round %s\n", tag.toStdString().c_str(),
                        currentRound().toStdString().c_str());
            if (signModal() == nullptr) {
                later([] { section()->m_sign_btn->click(); });
            }
        });
    for (const auto &signer : signers) {
        const QString *fp = signer.fp;
        step(
            tag + ": click the next cosigner",
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
                for (auto *row :
                     m->m_rows->findChildren<catalog::SelectRow *>(Qt::FindDirectChildrenOnly)) {
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
            step(
                tag + ": hot password", [] { return unsealShown() != nullptr; },
                [tag, name] {
                    check(signModal() != nullptr,
                          tag + ": the Sign modal stays shown under the password window");
                    check(visible<SigningFlow>() == nullptr,
                          tag + ": no signing spinner window under the password window");
                    if (!name.isEmpty()) {
                        shot(name);
                    }
                    enterPassword(unsealShown());
                });
            break;
        case RoundSigner::Kind::Ledger:
            step(
                tag + ": on the Ledger", [fp] { return onDeviceShown(*fp); },
                [tag, name] {
                    check(true, tag + ": the Ledger's row shows On Device in the Sign modal");
                    if (!name.isEmpty()) {
                        shot(name);
                    }
                });
            break;
        case RoundSigner::Kind::Coldcard:
            step(
                tag + ": on the Coldcard", [fp] { return onDeviceShown(*fp); },
                [tag, name] {
                    check(true, tag + ": the Coldcard's row shows On Device in the Sign modal");
                    if (!name.isEmpty()) {
                        shot(name);
                    }
                    approveColdcard();
                });
            step(
                tag + ": Coldcard approved", [] { return gCcDriver.finished(); },
                [tag] { checkColdcardApproved(tag + ": Coldcard approved"); });
            break;
        }
        step(
            tag + ": back on the Sign modal",
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
    step(
        "ready to broadcast",
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
            gBroadcastAnswered = false;
            QObject::connect(widget()->controller(), &MusigAccountController::broadcastDone,
                             gDriver, &Driver::onBroadcastDone, qontrol::UNIQUE);
            later([] { signModal()->m_broadcast_btn->click(); });
        });
    step(
        "broadcast", [] { return gBroadcastAnswered; },
        [] {
            check(gLastError.isEmpty(),
                  "broadcast: " + (gTxids.isEmpty() ? QString() : gTxids.last()) + gLastError);
        });
    step(
        "success message", [] { return visible<catalog::MessageModal>() != nullptr; },
        [success_shot] {
            shot(success_shot);
            chain("mine 1");
            auto txid = gTxids.last();
            auto conf = chain("conf " + txid);
            check(conf.toInt() >= 1, "spend " + txid + " confirmations " + conf);
            later([] { visible<catalog::MessageModal>()->accept(); });
        });
}

void addSteps() {
    // ===== Devices and keys =====
    // The Keys tab polls the devices while it shows.
    step(
        "Coldcard simulator socket", [] { return ColdcardSim::socketPresent(); },
        [] { AppController::get()->showKeysTab(); });
    step(
        "Ledger and Coldcard ready",
        [] { return signerReady(LEDGER_ID) && signerReady(COLDCARD_ID); },
        [] {
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
    step("import hot key A", always(),
         [] { gKeys->importHotKey(900, gEnv.seed_a, QStringLiteral("Hot"), PASSWORD); });
    step(
        "hot key A imported", [] { return gKeys->tabKeys().size() == 1; },
        [] {
            gFpA = keys::KeysModule::toQt(gKeys->tabKeys().first().fingerprint);
            std::printf("A %s\n", gFpA.toStdString().c_str());
        });
    // Both devices are turned into keys at once: the Coldcard starts while the Ledger
    // is still giving its xpubs.
    startDeviceKeySteps(LEDGER_ID, QStringLiteral("Ledger"), &gFpB,
                        QStringLiteral("02-register-ledger"));
    step(
        "the Ledger creating its key",
        [] { return keysTab() != nullptr && keysTab()->m_creations.size() == 1; },
        [] {
            check(true, "the Ledger is being turned into a key");
            shot("02a-keys-ledger-creating");
        });
    startDeviceKeySteps(COLDCARD_ID, QStringLiteral("Coldcard"), &gFpC, QString());
    // The Coldcard gives its xpubs at once, the Ledger waits for its 87' approval: the
    // Coldcard is a key while the Ledger is still creating its own.
    step(
        "the Coldcard a key while the Ledger still creates its own",
        [] { return visible<keys::modal::RegisterDevice>() == nullptr && hasUsbRoute(gFpC); },
        [] {
            auto ledger = signerById(LEDGER_ID);
            bool ledgerCreating =
                ledger.has_value() && keysTab()->m_creations.contains(ledger->instanceId);
            check(ledgerCreating && !hasUsbRoute(gFpB),
                  "the Ledger and the Coldcard create their keys in parallel: the Coldcard "
                  "is a key while the Ledger still creates its own");
            shot("02b-keys-two-devices-creating");
        });
    checkDeviceKeySteps(LEDGER_ID, QStringLiteral("Ledger"), &gFpB);
    checkDeviceKeySteps(COLDCARD_ID, QStringLiteral("Coldcard"), &gFpC);
    step(
        "keys tab", [] { return gKeys->tabKeys().size() == 3; },
        [] {
            check(!gFpB.isEmpty() && !gFpC.isEmpty() &&
                      QString::compare(gFpB, gFpA, Qt::CaseInsensitive) != 0 &&
                      QString::compare(gFpC, gFpA, Qt::CaseInsensitive) != 0 &&
                      QString::compare(gFpB, gFpC, Qt::CaseInsensitive) != 0,
                  "A, B and C are three keys: " + gFpA + " " + gFpB + " " + gFpC);
            shot("03-keys-a-hot-b-ledger-c-coldcard");
            gWindow->m_tab->setCurrentWidget(gWindow->m_menu_tab);
            Host::get()->refreshLauncher();
        });

    // ===== Creator =====
    step("launcher", always(), [] {
        shot("04-launcher");
        auto *b = launcherButton(TR("btn-create-account"), true);
        check(b != nullptr, "launcher has the create account button");
        later([b] { b->click(); });
    });
    step(
        "account type", [] { return visible<modal::AccountType>() != nullptr; },
        [] {
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
    step(
        "spend paths", [] { return wizardAt(CreatePolicy::View::Paths) != nullptr; },
        [] {
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
            auto hotLedger = pair(gFpA, gFpB);
            hotLedger.lock = musig::modal::LockKind::Relative;
            hotLedger.count = 1;
            hotLedger.unit = musig::modal::LockUnit::Blocks;
            m->m_paths = {pair(gFpB, gFpC), hotLedger};
            musig::modal::SpendPath recovery;
            recovery.name = QStringLiteral("Recovery");
            recovery.keys = QStringList{gFpC};
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
    step(
        "fetch page",
        [] {
            auto *m = wizardAt(CreatePolicy::View::Xpubs);
            return m != nullptr && m->m_asks.isEmpty() && !m->missingXpubs().isEmpty();
        },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Xpubs);
            auto missing = m->missingXpubs();
            check(missing == QStringList{gFpA},
                  "only the hot key's xpub is missing: " + missing.join(' '));
            auto *row = wizardRow(m->m_xpub_rows, missing.first());
            later([row] { emit row->clicked(); });
        });
    step(
        "fetch A password", [] { return unsealShown() != nullptr; },
        [] {
            check(visible<SigningFlow>() == nullptr,
                  "fetch A: no signing spinner window under the password window");
            enterPassword(unsealShown());
        });
    step(
        "xpubs fetched",
        [] {
            auto *m = wizardAt(CreatePolicy::View::Xpubs);
            return m != nullptr && m->m_asks.isEmpty() && m->missingXpubs().isEmpty();
        },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Xpubs);
            later([m] { m->onNextClicked(); });
        });
    step(
        "review", [] { return wizardAt(CreatePolicy::View::Review) != nullptr; },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Review);
            gDescriptor = m->m_descriptor;
            std::printf("descriptor %s\n", gDescriptor.toStdString().c_str());
            check(gDescriptor.contains("musig("), "musig descriptor compiled");
            for (const auto &fp : m->m_fingerprints) {
                if (fp == gFpB || fp == gFpC) {
                    gRegisterDevices.append(fp);
                }
            }
            std::printf("registration order %s\n",
                        m->m_fingerprints.join(' ').toStdString().c_str());
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
    step(
        "click both devices",
        [] {
            auto *m = wizardAt(CreatePolicy::View::Register);
            return m != nullptr && gRegisterDevices.size() == 2 &&
                   !m->askingAny(CreatePolicy::Ask::Register);
        },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Register);
            QList<catalog::SelectRow *> rows;
            for (const auto &fp : gRegisterDevices) {
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
    step(
        "both devices registering at once",
        [] {
            if (auto *u = unsealShown(); u != nullptr) {
                enterPassword(u);
            }
            auto *m = wizardAt(CreatePolicy::View::Register);
            if (m == nullptr) {
                return false;
            }
            return std::ranges::any_of(gKeys->m_device_registrations, [](const auto &registration) {
                return QString::compare(registration.fingerprint, gFpC, Qt::CaseInsensitive) == 0 &&
                       !registration.checking;
            });
        },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Register);
            check(m->asking(CreatePolicy::Ask::Register, gFpC),
                  "the Coldcard registers, clicked while the Ledger was registering");
            check(m->asking(CreatePolicy::Ask::Register, gFpB) || m->m_registered.contains(gFpB),
                  "the Ledger was asked too, not dropped for the Coldcard");
            std::printf("Ledger %s while the Coldcard enrolls\n",
                        m->m_registered.contains(gFpB) ? "registered" : "still registering");
            shot("08-creator-register-both");
            gEnrollSent.start();
        });
    // The Coldcard answers its enroll request at once, then asks its user: until the
    // user approves, the wizard keeps it registering.
    step(
        "Coldcard waits for its user", [] { return gEnrollSent.elapsed() > ENROLL_WAIT_MS; },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Register);
            check(!m->m_registered.contains(gFpC) && m->asking(CreatePolicy::Ask::Register, gFpC),
                  "the Coldcard still registering while its user has not approved");
            shot("08b-creator-register-coldcard-unconfirmed");
            approveColdcard();
        });
    step(
        "Coldcard enrolled", [] { return gCcDriver.finished(); },
        [] { checkColdcardApproved("the Coldcard enrolled the wallet"); });
    step(
        "every device registered",
        [] {
            auto *m = wizardAt(CreatePolicy::View::Register);
            return m != nullptr && m->m_asks.isEmpty() && m->m_registered.contains(gFpB) &&
                   m->m_registered.contains(gFpC) && m->m_registered.contains(gFpA);
        },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Register);
            auto hmac = MusigCreatorModule::get() == nullptr
                            ? QString()
                            : Host::get()
                                  ->keys(MusigCreatorModule::get())
                                  ->registrationHmac(plugin::Network::Regtest, gFpB, gDescriptor);
            static const QRegularExpression c_hex64(QStringLiteral("^[0-9a-f]{64}$"));
            check(c_hex64.match(hmac).hasMatch(),
                  "the Ledger HMAC is stored (" + QString::number(hmac.size()) + " chars)");
            check(m->m_register_next_btn->isEnabled(),
                  "Continue enabled once every device registered");
            shot("09b-creator-registered");
            later([m] { m->onNextClicked(); });
        });
    step(
        "verify address step", [] { return wizardAt(CreatePolicy::View::Verify) != nullptr; },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Verify);
            std::printf("first address %s\n", std::string(m->m_address.address).c_str());
            auto *row = wizardRow(m->m_verify_rows, gFpB);
            check(row != nullptr && row->isEnabled(), "check on the Ledger offered");
            markLedgerScreens();
            later([row] { emit row->clicked(); });
        });
    // A click sends the address straight to the device: no signer picker, no window.
    step(
        "verify on the Ledger",
        [] {
            auto *m = wizardAt(CreatePolicy::View::Verify);
            return m != nullptr && m->asking(CreatePolicy::Ask::Verify, gFpB);
        },
        [] {
            check(visible<SigningFlow>() == nullptr,
                  "no signer picker opened over the Verify step");
            shot("10-creator-verify-ledger");
        });
    step(
        "verified",
        [] {
            auto *m = wizardAt(CreatePolicy::View::Verify);
            return m != nullptr && m->m_asks.isEmpty();
        },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Verify);
            check(m->m_verified.contains(gFpB), "the Ledger row reads Verified");
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
    step(
        "electrum", [] { return wizardAt(CreatePolicy::View::Electrum) != nullptr; },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Electrum);
            m->m_electrum.input()->setText(gElectrum);
            m->onElectrumEdited();
            later([m] { m->onTestElectrum(); });
        });
    step(
        "electrum tested",
        [] {
            auto *m = wizardAt(CreatePolicy::View::Electrum);
            return m != nullptr && (m->m_electrum.state() == catalog::EndpointRow::State::Ok ||
                                    m->m_electrum.state() == catalog::EndpointRow::State::Failed);
        },
        [] {
            auto *m = wizardAt(CreatePolicy::View::Electrum);
            check(m->m_electrum.state() == catalog::EndpointRow::State::Ok,
                  "electrum test passes against electrs");
            shot("11b-creator-electrum");
            gInstancesBefore = dynamic_cast<HostImpl *>(Host::get())->liveInstances().size();
            later([m] { m->m_install_btn->click(); });
        });
    step(
        "launcher lists it",
        [] {
            return visible<CreatePolicy>() == nullptr && launcherButton(ACCOUNT, true) != nullptr;
        },
        [] {
            check(dynamic_cast<HostImpl *>(Host::get())->liveInstances().size() == gInstancesBefore,
                  "installed, not opened");
            shot("12-launcher-after-install");
            auto *b = launcherButton(ACCOUNT, true);
            later([b] { b->click(); });
        });

    // ===== Receive =====
    step(
        "account open", [] { return widget() != nullptr; },
        [] { widget()->controller()->receiveClicked(); });
    step(
        "receive shown", [] { return receive() != nullptr; },
        [] { later([] { emit receiveRow() -> generateRequested(); }); });
    step(
        "address generated", [] { return !receiveRow()->address().isEmpty(); },
        [] {
            std::printf("receive %s\n", receiveRow()->address().toStdString().c_str());
            shot("13-receive-generated");
            later([] { emit receiveRow() -> verifyRequested(); });
        });
    step(
        "receive verify picker", [] { return visible<SigningFlow>() != nullptr; },
        [] { pickRow(gFpC, "the Coldcard, to show the address"); });
    // The Coldcard answers with the address at once and keeps it on screen in a story.
    step(
        "receive on the Coldcard",
        [] {
            auto *f = visible<SigningFlow>();
            return usbWaiting(gFpC) || (f != nullptr && f->m_verify_result.has_value());
        },
        [] { approveColdcard(); });
    step(
        "Coldcard showed the address", [] { return gCcDriver.finished(); },
        [] { checkColdcardApproved("the Coldcard showed the receive address"); });
    step(
        "receive verified",
        [] {
            auto *f = visible<SigningFlow>();
            return f != nullptr && f->m_verify_result.has_value();
        },
        [] {
            auto *f = visible<SigningFlow>();
            check(f->m_verify_result.value() == SigningFlow::VerifyResult::Matched,
                  "the Coldcard matched the receive address");
            dismissColdcard();
        });
    step(
        "Coldcard story dismissed", [] { return gCcDriver.finished(); },
        [] {
            gCcDriver.result();
            shot("14-receive-verified-coldcard");
            later([] { visible<SigningFlow>()->accept(); });
        });
    // The same address on the Ledger, from Receive's picker.
    step(
        "receive verify on the Ledger", [] { return visible<SigningFlow>() == nullptr; },
        [] {
            markLedgerScreens();
            later([] { emit receiveRow() -> verifyRequested(); });
        });
    step(
        "receive verify picker 2", [] { return visible<SigningFlow>() != nullptr; },
        [] { pickRow(gFpB, "the Ledger, to show the address"); });
    step(
        "receive verified on the Ledger",
        [] {
            auto *f = visible<SigningFlow>();
            return f != nullptr && f->m_verify_result.has_value();
        },
        [] {
            auto *f = visible<SigningFlow>();
            check(f->m_verify_result.value() == SigningFlow::VerifyResult::Matched,
                  "the Ledger matched the receive address");
            shot("14b-receive-verified-ledger");
            later([f] { f->accept(); });
        });
    step(
        "fund", [] { return visible<SigningFlow>() == nullptr; },
        [] {
            auto address = receiveRow()->address();
            check(!chain("send " + address + " 0.02").isEmpty(), "funded");
            chain("mine 1");
        });
    step("coin confirmed", confirmedCoin, [] { widget()->controller()->sendClicked(); });

    // ===== Spend 1: the Ledger + Coldcard leaf =====
    step(
        "send shown", [] { return sendScreen() != nullptr && section() != nullptr; },
        [] {
            auto *payment = sendScreen()->m_outputs.begin().value();
            payment->m_address_input->setText(chain("newaddr"));
            payment->m_amount_input->setText("0.005");
        });
    step(
        "leaf picker", [] { return section()->m_leaf_combo->count() > 0; },
        [] {
            auto *s = section();
            int bc = leafOf(s, {gFpB, gFpC});
            check(bc >= 0, "a Ledger + Coldcard leaf");
            s->m_leaf_combo->setCurrentIndex(bc);
            sendScreen()->onProcess();
            later([s] { s->m_leaf_combo->showPopup(); });
        });
    step("leaf popup", always(), [] {
        shot("15-send-leaf-picker");
        section()->m_leaf_combo->hidePopup();
    });
    step(
        "sign", [] { return section()->m_sign_btn->isEnabled(); },
        [] { later([] { section()->m_sign_btn->click(); }); });
    step(
        "prepared", [] { return signModal() != nullptr; },
        [] {
            check(currentRound() == "Nonces", "prepared: round 1");
            shot("16-sign-modal-round1");
        });
    using Kind = RoundSigner::Kind;
    // Round 1 on both devices at once: both rows clicked in the same pass, the
    // Coldcard asked while the Ledger works on its nonce.
    step(
        "round 1: both devices clicked",
        [] {
            auto *b = cosignerRow(gFpB);
            auto *c = cosignerRow(gFpC);
            return b != nullptr && c != nullptr && b->isEnabled() && c->isEnabled();
        },
        [] {
            auto *b = cosignerRow(gFpB);
            auto *c = cosignerRow(gFpC);
            later([b, c] {
                emit b->clicked();
                emit c->clicked();
            });
        });
    step(
        "round 1: both on their device", [] { return onDeviceShown(gFpB) && onDeviceShown(gFpC); },
        [] {
            check(true, "round 1: the Ledger and the Coldcard both On Device at once");
            shot("17-round1-both-devices");
            approveColdcard();
        });
    step(
        "round 1: Coldcard approved", [] { return gCcDriver.finished(); },
        [] { checkColdcardApproved("round 1: Coldcard approved"); });
    step(
        "round 1: both answered",
        [] { return signModal() != nullptr && widget()->controller()->signingKeys().isEmpty(); },
        [] {
            auto *m = signModal();
            bool notice = m->m_notice->isVisibleTo(m);
            auto text = m->m_notice->m_text != nullptr ? m->m_notice->m_text->text() : QString();
            check(!notice,
                  "round 1: both nonces merged, no notice" + (notice ? ": " + text : QString()));
            check(currentRound() == "PartialSigs", "round 1: every nonce in: " + currentRound());
            shot("18-round1-both-nonces");
            later([m] { m->m_continue_btn->click(); });
        });
    step(
        "after round 1", [] { return signModal() != nullptr && signModal()->m_partial; },
        [] {
            check(currentRound() == "PartialSigs", "round 1 done: round 2 next");
            shot("19-after-round1");
            // Closed, the modal keeps the session: Sign opens it again at its round.
            later([] { signModal()->reject(); });
        });
    // phase 29's lost session: the simulator restarts with the same seed and nothing
    // enrolled (--eff). Receive's verify picker stays open meanwhile: its Coldcard row
    // follows the device, gone then back.
    step(
        "verify picker open", [] { return signModal() == nullptr; },
        [] {
            widget()->controller()->receiveClicked();
            later([] { emit receiveRow() -> verifyRequested(); });
        });
    step(
        "verify picker lists the Coldcard",
        [] { return pickerStatus(gFpC) == keys::flow::SignerStatus::Ready; },
        [] {
            check(true, "the verify picker lists the Coldcard ready");
            gColdcard.stop();
        });
    step(
        "Coldcard dropped",
        [] {
            return !signerById(COLDCARD_ID).has_value() &&
                   pickerStatus(gFpC) == keys::flow::SignerStatus::Disconnected;
        },
        [] {
            check(true, "the hwi service dropped the Coldcard, its picker row Not Connected");
            shot("19b-verify-picker-coldcard-gone");
            gColdcard.start();
        });
    step(
        "picker sees the Coldcard back",
        [] { return pickerStatus(gFpC) == keys::flow::SignerStatus::Ready; },
        [] {
            check(true, "the restarted Coldcard is ready again in the picker");
            shot("19c-verify-picker-coldcard-back");
            later([] {
                visible<SigningFlow>()->reject();
                widget()->controller()->sendClicked();
            });
        });
    step(
        "Coldcard reacquired", [] { return signerReady(COLDCARD_ID); },
        [] {
            auto fp = signerById(COLDCARD_ID)->signer->fingerprint();
            check(QString::compare(fp, gFpC, Qt::CaseInsensitive) == 0,
                  "the restarted Coldcard is key C again: " + fp);
            gCcDriver.start({"story"});
        });
    step(
        "restarted Coldcard story", [] { return gCcDriver.finished(); },
        [] {
            auto [code, output] = gCcDriver.result();
            check(code == 0 && output.isEmpty(), "the restarted Coldcard shows no story");
            AppController::get()->showKeysTab();
        });
    step(
        "open key C", [] { return keysTabRow("keyFingerprint", gFpC) != nullptr; },
        [] {
            later([] {
                if (auto *row = keysTabRow("keyFingerprint", gFpC); row != nullptr) {
                    emit row->clicked();
                }
            });
        });
    step(
        "key C checked on the Coldcard",
        [] {
            auto *d = visible<keys::modal::KeyDetail>();
            return d != nullptr && d->m_registrations.contains(gDescriptor) &&
                   d->m_registrations.value(gDescriptor) ==
                       keys::modal::Registration::NotRegistered;
        },
        [] {
            auto *d = visible<keys::modal::KeyDetail>();
            QPushButton *button = nullptr;
            for (auto *b : d->findChildren<QPushButton *>()) {
                if (b->property("descriptor").toString() == gDescriptor && b->isVisible()) {
                    button = b;
                }
            }
            check(button != nullptr && button->isEnabled(),
                  "key C's detail offers to register the account on the Coldcard");
            if (button != nullptr) {
                later([button] { button->click(); });
            }
        });
    step(
        "enroll again on the Coldcard",
        [] {
            auto *d = visible<keys::modal::KeyDetail>();
            if (d == nullptr) {
                return false;
            }
            auto state = d->m_registrations.value(gDescriptor);
            return (state == keys::modal::Registration::Registering && d->m_register_req != 0) ||
                   state == keys::modal::Registration::Registered;
        },
        [] { approveColdcard(); });
    step(
        "Coldcard enrolled again", [] { return gCcDriver.finished(); },
        [] { checkColdcardApproved("the restarted Coldcard enrolled the wallet"); });
    step(
        "key C registered",
        [] {
            auto *d = visible<keys::modal::KeyDetail>();
            return d != nullptr &&
                   d->m_registrations.value(gDescriptor) == keys::modal::Registration::Registered;
        },
        [] {
            // Registered once the Coldcard lists the wallet, after its approval.
            shot("20-reenroll-coldcard");
            auto *d = visible<keys::modal::KeyDetail>();
            later([d] { d->accept(); });
        });
    step(
        "back to the account", [] { return visible<keys::modal::KeyDetail>() == nullptr; },
        [] {
            for (const auto &tab : gWindow->m_tabs) {
                if (tab.second == widget() || tab.second->isAncestorOf(widget())) {
                    gWindow->m_tab->setCurrentWidget(tab.second);
                }
            }
        });
    signRoundSteps("round 2 lost",
                   {{&gFpB, Kind::Ledger, QString()}, {&gFpC, Kind::Coldcard, QString()}}, true);
    step("lost session shot", always(), [] {
        shot("21-lost-session-notice");
        dismissColdcard();
    });
    step(
        "lost session story dismissed", [] { return gCcDriver.finished(); },
        [] { gCcDriver.result(); });
    signRoundSteps("round 1 again",
                   {{&gFpB, Kind::Ledger, "22-round1-again-ledger-usb"},
                    {&gFpC, Kind::Coldcard, "22-round1-again-coldcard-usb"}},
                   false);
    step("round 1 again done", always(),
         [] { check(currentRound() == "PartialSigs", "round 1 again done: round 2 next"); });
    signRoundSteps("round 2",
                   {{&gFpB, Kind::Ledger, "23-round2-ledger-usb"},
                    {&gFpC, Kind::Coldcard, "24-round2-coldcard-usb"}},
                   false);
    broadcastSteps("25-ready-to-broadcast", "26-broadcast-success");

    // ===== Spend 2: the Ledger + hot leaf =====
    step(
        "change confirmed", [] { return visible<catalog::Modal>() == nullptr && confirmedCoin(); },
        [] { widget()->controller()->sendClicked(); });
    step(
        "send again", [] { return section() != nullptr && !section()->m_session; },
        [] {
            auto *payment = sendScreen()->m_outputs.begin().value();
            payment->m_address_input->setText(chain("newaddr"));
            payment->m_amount_input->setText("0.004");
        });
    step(
        "hot leaf", [] { return section()->m_leaf_combo->count() > 0; },
        [] {
            auto *s = section();
            int ab = leafOf(s, {gFpA, gFpB});
            check(ab >= 0, "a Ledger + hot leaf");
            s->m_leaf_combo->setCurrentIndex(ab);
            sendScreen()->onProcess();
        });
    step(
        "sign 2", [] { return section()->m_sign_btn->isEnabled(); },
        [] {
            shot("27-send-ledger-hot-leaf");
            later([] { section()->m_sign_btn->click(); });
        });
    step(
        "prepared 2", [] { return signModal() != nullptr; },
        [] { check(currentRound() == "Nonces", "prepared 2: round 1"); });
    signRoundSteps("spend 2 round 1",
                   {{&gFpA, Kind::Hot, "28-round1-hot-password"}, {&gFpB, Kind::Ledger, QString()}},
                   false);
    step("spend 2 round 1 done", always(),
         [] { check(currentRound() == "PartialSigs", "spend 2 round 1 done: round 2 next"); });
    signRoundSteps("spend 2 round 2",
                   {{&gFpA, Kind::Hot, QString()}, {&gFpB, Kind::Ledger, "29-round2-ledger-usb"}},
                   false);
    broadcastSteps(QString(), "30-broadcast-success-2");

    // ===== Spend 3: the Coldcard alone, on its timelocked leaf =====
    step(
        "mine past the recovery timelock", [] { return visible<catalog::Modal>() == nullptr; },
        [] {
            chain(QStringLiteral("mine %1").arg(RECOVERY_BLOCKS + 1));
            widget()->controller()->sendClicked();
        });
    step(
        "send 3", [] { return section() != nullptr && !section()->m_session; },
        [] {
            auto *payment = sendScreen()->m_outputs.begin().value();
            payment->m_address_input->setText(chain("newaddr"));
            payment->m_amount_input->setText("0.002");
        });
    step(
        "Coldcard leaf open",
        [] {
            auto *s = section();
            int c = leafOf(s, {gFpC});
            return c >= 0 && s->m_leaves.leaves[c].spendable;
        },
        [] {
            auto *s = section();
            int c = leafOf(s, {gFpC});
            check(true, "the Coldcard's recovery leaf is spendable");
            s->m_leaf_combo->setCurrentIndex(c);
            sendScreen()->onProcess();
        });
    step(
        "sign 3", [] { return section()->m_sign_btn->isEnabled(); },
        [] {
            shot("31-send-coldcard-leaf");
            later([] { section()->m_sign_btn->click(); });
        });
    step(
        "prepared 3", [] { return signModal() != nullptr; },
        [] {
            auto *m = signModal();
            check(currentRound() == "NotMusig", "prepared 3: a single-key leaf, no MuSig2 round");
            // The path's signer first, the wallet's other keys after it, greyed.
            QStringList order;
            for (int i = 0; i < m->m_rows->layout()->count(); ++i) {
                auto *row =
                    qobject_cast<catalog::SelectRow *>(m->m_rows->layout()->itemAt(i)->widget());
                if (row != nullptr) {
                    order.append(row->property("fingerprint").toString() +
                                 (row->isEnabled() ? "" : " (greyed)"));
                }
            }
            check(order.size() == 3 && order.first() == gFpC && order.at(1).endsWith("(greyed)") &&
                      order.at(2).endsWith("(greyed)"),
                  "every key listed, the Coldcard first, the others greyed: " + order.join(", "));
            shot("32-sign-coldcard-leaf");
            later([] { emit cosignerRow(gFpC)->clicked(); });
        });
    step(
        "spend 3: on the Coldcard", [] { return onDeviceShown(gFpC); },
        [] {
            shot("33-coldcard-leaf-on-device");
            approveColdcard();
        });
    step(
        "spend 3: Coldcard approved", [] { return gCcDriver.finished(); },
        [] { checkColdcardApproved("spend 3: Coldcard approved"); });
    step(
        "spend 3: signed",
        [] {
            auto *m = signModal();
            return m != nullptr && widget()->controller()->signingKeys().isEmpty();
        },
        [] {
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
    step(
        "settings", [] { return visible<catalog::Modal>() == nullptr; },
        [] { widget()->controller()->settingsClicked(); });
    step(
        "electrum height",
        [] {
            auto *settings = widget()->findChild<account::view::Settings *>();
            bool ok = false;
            return settings != nullptr && settings->isVisible() &&
                   (settings->row(Endpoint::Electrum)->height->text().section(' ', -1).toUInt(&ok),
                    ok);
        },
        [] {
            auto *settings = widget()->findChild<account::view::Settings *>();
            auto shown = settings->row(Endpoint::Electrum)->height->text();
            auto tip = chain("mine 0");
            check(shown == TR("settings-height").arg(tip),
                  "Settings show the Electrum block height " + shown + " (chain " + tip + ")");
        });
    step(
        "register descriptor button",
        [] {
            auto *extras = widget()->findChild<musig::SettingsExtras *>();
            return extras != nullptr && extras->m_register_btn->isVisible();
        },
        [] {
            shot("36a-settings-register-descriptor");
            auto *extras = widget()->findChild<musig::SettingsExtras *>();
            later([extras] { extras->m_register_btn->click(); });
        });
    step(
        "register descriptor checked",
        [] {
            auto *m = visible<musig::modal::RegisterDescriptor>();
            return m != nullptr && m->m_requests.isEmpty() &&
                   m->m_states.value(gFpB) != musig::modal::RegisterDescriptor::State::Checking;
        },
        [] {
            auto *m = visible<musig::modal::RegisterDescriptor>();
            using State = musig::modal::RegisterDescriptor::State;
            check(m->m_states.value(gFpB) == State::Registered,
                  "Register Descriptor: the Ledger holds the account");
            check(m->m_states.value(gFpC) == State::Registered,
                  "Register Descriptor: the Coldcard holds the account");
            check(m->m_states.value(gFpA) == State::NotNeeded,
                  "Register Descriptor: the hot key needs no registration");
            shot("36b-register-descriptor");
            later([m] { m->accept(); });
        });

    // ===== Electrum going away =====
    // The server closes the connection (a restart): the account sees it and reconnects.
    step(
        "electrum connected", [] { return electrumUp(); },
        [] {
            check(chain("relay drop").toInt() >= 1, "the relay dropped the account's connection");
            gSince.start();
        });
    step(
        "electrum drop seen", [] { return !electrumUp(); },
        [] {
            std::printf("drop seen after %lld ms\n", gSince.elapsed());
            gSince.start();
        });
    step(
        "electrum reconnected", [] { return electrumUp(); },
        [] {
            check(true, QStringLiteral("the account reconnected after the server closed, in %1 ms")
                            .arg(gSince.elapsed()));
            // A link dead with no close (sleep, NAT, wifi lost): only the keepalive's
            // unanswered ping can tell.
            check(chain("relay mute") == "muted", "the relay went silent");
            gSince.start();
        });
    step(
        "electrum silent death seen", [] { return !electrumUp(); },
        [] {
            check(gSince.elapsed() < KEEPALIVE_DETECT_MS,
                  QStringLiteral("the dead link was caught in %1 ms").arg(gSince.elapsed()));
            check(chain("relay unmute") == "unmuted", "the relay relays again");
            gSince.start();
        });
    step(
        "electrum reconnected after the dead link", [] { return electrumUp(); },
        [] {
            check(true, QStringLiteral("the account reconnected after the dead link, in %1 ms")
                            .arg(gSince.elapsed()));
        });

    // ===== End =====
    step(
        "history", [] { return visible<catalog::Modal>() == nullptr; },
        [] { widget()->controller()->historyClicked(); });
    step("end", always(), [] {
        shot("37-history-three-spends");
        check(gTxids.size() == 3, "three spends broadcast");
        for (const auto &txid : gTxids) {
            std::printf("txid %s\n", txid.toStdString().c_str());
        }
        std::printf("shots: %d\nfailures: %d\n", gShotCount, gFailures);
        gDriver->finish(gFailures == 0 ? 0 : 1);
    });
}

// Waits up to `ms` for `done`, the events running meanwhile.
auto waitFor(int ms, const std::function<bool()> &done) -> bool {
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) {
            return false;
        }
        spin(100);
    }
    return true;
}

// The app state (ws-plaude/silent#37), created at the first start with the Coldcard
// as its unlock device through the setup screens, then locked and unlocked with it.
auto openStateWithColdcard() -> bool {
    // The simulators run a test network app: the app asks on mainnet.
    auto *setup = new state::StateSetup(Network::Regtest);
    gFirstWindow = setup;
    setup->move(0, 0);
    setup->show();
    shot("00a-state-setup-choice");
    emit setup->m_device_choice->clicked();
    auto *flow = qobject_cast<state::DeviceFlow *>(setup->m_flow);

    QString coldcardId;
    QString coldcardFp;
    bool seen = waitFor(60000, [&] {
        for (const auto &device : flow->m_devices->devices()) {
            QString kind = QString::fromStdString(std::string(device.kind));
            if (kind.startsWith(QStringLiteral("coldcard")) &&
                device.state == HwiDeviceState::Supported) {
                coldcardId = QString::fromStdString(std::string(device.id));
                coldcardFp = QString::fromStdString(std::string(device.fingerprint));
                return true;
            }
        }
        return false;
    });
    check(seen, "state setup: the Coldcard is listed ready");
    if (!seen) {
        return false;
    }
    for (auto *row : flow->m_device_rows->findChildren<catalog::SelectRow *>()) {
        if (row->property("deviceId").toString() == coldcardId) {
            emit row->clicked();
        }
    }
    check(flow->m_device_continue->isEnabled(), "state setup: Continue once picked");
    shot("00b-state-setup-device");
    flow->m_device_continue->click();
    bool created = waitFor(60000, [&] { return !setup->isVisible(); });
    check(created && setup->result() == QDialog::Accepted,
          "state setup: created with the Coldcard key");
    auto info = ::state_unlock_info();
    QString path = QString::fromStdString(std::string(info.path));
    check(info.exists && info.device && path.startsWith(QStringLiteral("m/138'/1936225652'/")),
          "the device path " + path + " is read from unlock.bip138");
    check(!QFile::exists(QDir::homePath() + "/.silent/unlock.json"), "no unlock.json");
    QFile checkFile(QDir::homePath() + "/.silent/unlock.bip138");
    checkFile.open(QIODevice::ReadOnly);
    QByteArray sealed = checkFile.readAll();
    check(sealed.startsWith("BIP138") && !sealed.contains(coldcardFp.toLatin1()),
          "unlock.bip138 is sealed and does not name the Coldcard");
    delete setup;

    ::state_lock();
    auto *unlock = new state::DeviceUnlock(info, Network::Regtest);
    gFirstWindow = unlock;
    unlock->move(0, 0);
    unlock->show();
    shot("00c-state-unlock");
    bool unlocked = waitFor(60000, [&] { return !unlock->isVisible(); });
    check(unlocked && unlock->result() == QDialog::Accepted,
          "state unlock: the Coldcard's key opens the state");
    delete unlock;
    gFirstWindow = nullptr;
    return unlocked;
}

} // namespace

auto main(int argc, char *argv[]) -> int {
    setvbuf(stdout, nullptr, _IONBF, 0);
    gEnv = Env{
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
    QDir().mkpath(gEnv.run_dir + "/shots");

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
    gChain = new LineProcess(gEnv.chain_bin, gEnv.run_dir + "/chain.stderr.log");
    auto ready = gChain->start(CHAIN_START_TIMEOUT_MS);
    check(ready.startsWith("READY "), "chain up: " + ready);
    if (!ready.startsWith("READY ")) {
        return 1;
    }
    // Through the chain's relay: the run can make the server go away under the app.
    gElectrum = "tcp://127.0.0.1:" + chain("relay");
    std::printf("electrum %s (relaying %s)\n", gElectrum.toStdString().c_str(),
                ready.toStdString().c_str());
    gColdcard.start();

    Theme::init();
    Theme::get()->apply();
    i18n::I18nManager::get()->init(&app);
    if (!openStateWithColdcard()) {
        std::printf("FAIL: the app state did not open, stopping\n");
        std::fflush(stdout);
        std::_Exit(1);
    }
    AppController::init();
    auto *controller = AppController::get();
    Host::get();
    controller->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    controller->registerBuiltinPlugin(std::make_unique<MusigPlugin>());
    controller->registerBuiltinPlugin(std::make_unique<HwiPlugin>());
    auto keysModule = std::make_unique<keys::KeysModule>();
    gKeys = keysModule.get();
    controller->registerBuiltinPlugin(std::move(keysModule));
    gWindow = new MainWindow();
    controller->start(gWindow);
    controller->initState();
    gWindow->move(0, 0);
    gWindow->resize(WINDOW_WIDTH, WINDOW_HEIGHT);
    gWindow->show();
    gWindow->activateWindow();
    spin(1500);
    // Regtest through the Keys tab's own network selector, as a user picks it.
    auto *tab = keysTab();
    check(tab != nullptr, "keys tab");
    int regtest = tab->m_network_selector->findData(static_cast<int>(Network::Regtest));
    tab->m_network_selector->setCurrentIndex(regtest);
    tab->onNetworkChanged();

    gDriver = new Driver;
    addSteps();
    gDriver->start();
    QApplication::exec();
    int code = gDriver->exitCode();
    // The app's teardown waits on device and scanner threads the run does not need
    // stopped cleanly: the result is final here.
    std::fflush(stdout);
    std::_Exit(code);
}

#include "main.moc"
