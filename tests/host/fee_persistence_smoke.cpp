#include "fee_persistence_smoke.h"

#include "host/Host.h"
#include "interfaces/host.h"

#include <QChar>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QtLogging>
#include <algorithm>
#include <atomic>
#include <common.h>
#include <silent.h>
#include <string>
#include <vector>

#ifdef Q_OS_LINUX
#include <QDir>
#include <QFile>
#include <memory>
#include <sys/file.h>
#endif

namespace smoke {
namespace {
constexpr int TICK_MS = 5;
#ifdef Q_OS_LINUX
constexpr int HELD_MS = 500;
constexpr int QUIT_WRITE_FAILURES = 1;
#else
constexpr int QUIT_WRITE_FAILURES = 0;
#endif
constexpr int RESPONSIVE_MS = 200;
constexpr int WRITE_TIMEOUT_MS = 5000;
std::atomic<int> gFeeWriteWarnings{0};
std::atomic<int> gFeeWriteRefusals{0};
std::atomic<int> gInvalidFeeModes{0};
QtMessageHandler gPreviousHandler = nullptr;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("fee persistence: %s", message);
    }
}

void captureFeeWarning(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg &&
        message.startsWith(QStringLiteral("Host: failed to persist fee preferences:"))) {
        ++gFeeWriteWarnings;
    }
    if (type == QtWarningMsg &&
        message == QStringLiteral("Host: refusing fee preferences during shutdown")) {
        ++gFeeWriteRefusals;
    }
    if (type == QtWarningMsg &&
        message.startsWith(QStringLiteral("Host: refusing invalid fee mode:"))) {
        ++gInvalidFeeModes;
    }
    if (gPreviousHandler != nullptr) {
        gPreviousHandler(type, context, message);
    }
}

class FeeWarningCapture final {
public:
    FeeWarningCapture() {
        gFeeWriteWarnings.store(0);
        gFeeWriteRefusals.store(0);
        gInvalidFeeModes.store(0);
        gPreviousHandler = qInstallMessageHandler(captureFeeWarning);
    }

    ~FeeWarningCapture() {
        qInstallMessageHandler(gPreviousHandler);
        gPreviousHandler = nullptr;
    }
};

#ifdef Q_OS_LINUX
// The independent unlock bounds a regression that blocks selectFees on the GUI thread.
class HeldFeeState final {
public:
    HeldFeeState() {
        m_file.setFileName(QDir::homePath() + QStringLiteral("/.silent/app.lock"));
        require(m_file.open(QIODevice::ReadWrite), "the real app.lock opens");
        require(::flock(m_file.handle(), LOCK_EX) == 0, "the fixture holds the real app.lock");
        auto fd = m_file.handle();
        auto released = m_released;
        m_unlocker = QThread::create([fd, released] {
            QThread::msleep(HELD_MS);
            released->store(true);
            require(::flock(fd, LOCK_UN) == 0, "the independent thread releases app.lock");
        });
        m_unlocker->start();
    }

    ~HeldFeeState() {
        m_unlocker->wait();
        delete m_unlocker;
    }

    [[nodiscard]] auto released() const -> bool {
        return m_released->load();
    }

private:
    QFile m_file;
    QThread *m_unlocker = nullptr;
    std::shared_ptr<std::atomic<bool>> m_released = std::make_shared<std::atomic<bool>>(false);
};
#endif

void processFor(int duration_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < duration_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
}

void waitForWrites(const FeeWriteProbe &probe, int count) {
    QElapsedTimer timer;
    timer.start();
    while (probe.outcomes().size() < count && timer.elapsed() < WRITE_TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
    require(probe.outcomes().size() == count, "every accepted fee write reports its result");
}

void requirePersisted(const FeePreferences &preferences) {
    auto stored = ::app_fee_preferences();
    require(stored.mode == static_cast<quint8>(preferences.mode),
            "the persisted fee mode matches the final accepted write");
    require(QString::fromStdString(std::string(stored.source)) == preferences.source,
            "the persisted fee source matches the final accepted write");
}

#ifdef Q_OS_LINUX
auto feeTempPath() -> QString {
    return QDir::homePath() + QStringLiteral("/.silent/app.tmp");
}
#endif

void settleSingletonWrites() {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the singleton provides the fee persistence barrier");
    FeeWriteProbe probe(host);
    auto source = QStringLiteral("fee-persistence-start");
    host->selectFees({.mode = FeeMode::Manual, .source = source});
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < WRITE_TIMEOUT_MS) {
        for (const auto &outcome : probe.outcomes()) {
            if (outcome.source == source) {
                require(outcome.error.isEmpty(), "earlier singleton writes drain before fixtures");
                return;
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, TICK_MS);
        QThread::msleep(1);
    }
    require(false, "the singleton fee persistence barrier completes before fixtures");
}

void fifoAndResponsiveGui() {
    HostImpl host;
    host.selectPrice(QStringLiteral("fee-price-preserved"), QStringLiteral("USD"));
    FeeWriteProbe probe(&host);
    QList<FeePreferences> selections{
        {.mode = FeeMode::Default, .source = QStringLiteral("fee-first")},
        {.mode = FeeMode::Default, .source = QStringLiteral("fee-first")},
        {.mode = FeeMode::Smart, .source = QStringLiteral("fee-second")},
        {.mode = FeeMode::Manual, .source = QStringLiteral("fee-last")}};
#ifdef Q_OS_LINUX
    HeldFeeState held;
#endif
    QElapsedTimer selectionTimer;
    selectionTimer.start();
    for (const auto &preferences : selections) {
        host.selectFees(preferences);
        require(host.selectedFees().preferences == preferences,
                "runtime fees change immediately before persistence completes");
    }
    require(selectionTimer.elapsed() < RESPONSIVE_MS,
            "rapid fee selections return without waiting for persistence");
#ifdef Q_OS_LINUX
    processFor(RESPONSIVE_MS);
    require(!held.released(), "the contention checks run before the lock is released");
    require(probe.ticks() >= 10, "GUI timer events run while the fee write waits for app.lock");
    require(probe.outcomes().empty(), "no fee write finishes against the held app.lock");
#endif
    waitForWrites(probe, 4);
    for (qsizetype i = 0; i < selections.size(); ++i) {
        const auto &outcome = probe.outcomes().at(i);
        const auto &selection = selections.at(i);
        require(outcome.mode == static_cast<quint8>(selection.mode) &&
                    outcome.source == selection.source && outcome.error.isEmpty(),
                "all four writes, including the duplicate, persist successfully in FIFO order");
    }
    requirePersisted(selections.last());
    require(std::string(::app_price_source()) == "fee-price-preserved" &&
                std::string(::app_price_currency()) == "USD",
            "fee persistence preserves price selection");
}

void reentrantSelection() {
    HostImpl host;
    FeeWriteProbe probe(&host);
    FeeReentrantSelection reentrant(&host);
    FeePreferences first{.mode = FeeMode::Default, .source = QStringLiteral("fee-reentrant-first")};
    FeePreferences newer{.mode = FeeMode::Smart, .source = QStringLiteral("fee-reentrant-newer")};
    host.selectFees(first);
    require(host.selectedFees().preferences == newer,
            "the direct notification's newer selection remains after the outer call returns");
    waitForWrites(probe, 2);
    require(probe.outcomes().at(0).mode == static_cast<quint8>(first.mode) &&
                probe.outcomes().at(0).source == first.source &&
                probe.outcomes().at(0).error.isEmpty(),
            "the outer selection persists first despite reentry from its notification");
    require(probe.outcomes().at(1).mode == static_cast<quint8>(newer.mode) &&
                probe.outcomes().at(1).source == newer.source &&
                probe.outcomes().at(1).error.isEmpty(),
            "the reentrant selection persists second in acceptance order");
    require(host.selectedFees().preferences == newer,
            "worker completions preserve the newer reentrant runtime selection");
    requirePersisted(newer);
}

void queuedSourceOwnsItsMemory() {
    HostImpl host;
    FeeWriteProbe probe(&host);
    auto expected = QStringLiteral("fee-owned-source");
    std::vector<QChar> backing(expected.cbegin(), expected.cend());
    auto borrowed = QString::fromRawData(backing.data(), static_cast<qsizetype>(backing.size()));
    host.selectFees({.mode = FeeMode::Default, .source = QStringLiteral("fee-ownership-first")});
    host.selectFees({.mode = FeeMode::Smart, .source = borrowed});
    require(probe.outcomes().empty(), "the borrowed source is queued behind the first write");
    // Keep the allocation live but corrupt it, making a shallow copy fail without a dangling read.
    std::ranges::fill(backing, QChar(u'x'));
    borrowed.clear();
    waitForWrites(probe, 2);
    require(probe.outcomes().last().source == QStringLiteral("fee-owned-source") &&
                probe.outcomes().last().error.isEmpty(),
            "the queued write owns its source instead of retaining caller backing memory");
    FeePreferences last{.mode = FeeMode::Smart, .source = QStringLiteral("fee-owned-source")};
    require(host.selectedFees().preferences == last,
            "caller backing memory changes do not alter the runtime selection");
    requirePersisted(last);
}

void invalidModeRejected() {
    HostImpl host;
    FeeWriteProbe probe(&host);
    FeeWarningCapture warnings;
    auto before = host.selectedFees().preferences;
    host.selectFees({.mode = static_cast<FeeMode>(255), .source = QStringLiteral("fee-invalid")});
    require(gInvalidFeeModes.load() == 1, "an invalid mode is explicitly warned before enqueue");
    require(host.selectedFees().preferences == before,
            "an invalid mode does not change runtime preferences");
    require(host.findChildren<QThread *>().empty(), "an invalid mode starts no persistence worker");
    processFor(50);
    require(probe.outcomes().empty(), "an invalid mode leaves no queued persistence result");
    FeePreferences valid{.mode = FeeMode::Default, .source = QStringLiteral("fee-after-invalid")};
    host.selectFees(valid);
    waitForWrites(probe, 1);
    require(probe.outcomes().first().error.isEmpty(),
            "a valid selection succeeds after rejecting an invalid mode");
    requirePersisted(valid);
}

#ifdef Q_OS_LINUX
void failureAndRecovery() {
    HostImpl host;
    FeeWriteProbe probe(&host);
    FeeWarningCapture warnings;
    auto before = host.selectedFees().preferences;
    auto temp = feeTempPath();
    require(!QFile::exists(temp), "the failure fixture does not overwrite an existing temp");
    require(QDir().mkdir(temp), "a directory blocks the real fee persistence temp file");
    host.selectFees({.mode = FeeMode::Default, .source = QStringLiteral("fee-fails-first")});
    FeePreferences newer{.mode = FeeMode::Smart, .source = QStringLiteral("fee-fails-newer")};
    host.selectFees(newer);
    waitForWrites(probe, 2);
    require(!probe.outcomes().at(0).error.isEmpty() && !probe.outcomes().at(1).error.isEmpty(),
            "both blocked fee writes report explicit backend errors");
    require(gFeeWriteWarnings.load() == 2, "each failed fee write is explicitly logged by Host");
    require(host.selectedFees().preferences == newer,
            "an older persistence failure does not roll back a newer runtime selection");
    // This fixture fails opening app.tmp, before any replacement.
    requirePersisted(before);
    require(QDir().rmdir(temp), "removing the empty fixture restores fee persistence");
    FeePreferences recovered{.mode = FeeMode::Default, .source = QStringLiteral("fee-recovered")};
    host.selectFees(recovered);
    waitForWrites(probe, 3);
    require(probe.outcomes().last().error.isEmpty(), "a later write succeeds after failure");
    require(gFeeWriteWarnings.load() == 2, "a recovered write adds no failure warning");
    requirePersisted(recovered);
}
#endif

void destructionDrains() {
    auto *host = new HostImpl();
    QPointer<HostImpl> hostGuard(host);
    FeeWriteProbe probe(host);
#ifdef Q_OS_LINUX
    HeldFeeState held;
#endif
    host->selectFees({.mode = FeeMode::Default, .source = QStringLiteral("fee-drain-first")});
    host->selectFees({.mode = FeeMode::Smart, .source = QStringLiteral("fee-drain-second")});
    FeePreferences last{.mode = FeeMode::Manual, .source = QStringLiteral("fee-drain-last")};
    host->selectFees(last);
    QPointer<QThread> worker(host->findChild<QThread *>());
    require(worker != nullptr, "the Host owns the running fee worker");
    delete host;
#ifdef Q_OS_LINUX
    require(held.released(), "Host destruction waits for the held persistence lock");
#endif
    require(hostGuard == nullptr && worker == nullptr,
            "Host destruction joins and deletes its worker");
    requirePersisted(last);
    processFor(50);
    require(probe.outcomes().empty(),
            "queued worker completions cannot call back through the destroyed Host");
}

void singletonQuitDrains() {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the real singleton provides the quit drain");
    QPointer<HostImpl> hostGuard(host);
    FeeWriteProbe probe(host);
    FeeWarningCapture warnings;
#ifdef Q_OS_LINUX
    auto temp = feeTempPath();
    require(!QFile::exists(temp), "the quit fixture does not overwrite an existing temp");
    require(QDir().mkdir(temp), "the quit fixture blocks the first persistence write");
#endif
    FeeQuitDriver driver(host);
    QTimer quitTimer;
    quitTimer.setSingleShot(true);
    QObject::connect(&quitTimer, &QTimer::timeout, &driver, &FeeQuitDriver::onQuitRequested,
                     qontrol::UNIQUE);
    quitTimer.start(TICK_MS);
    require(QCoreApplication::exec() == 0, "the real application quit path returns");
    require(hostGuard != nullptr && Host::get() == host,
            "aboutToQuit drains the singleton without relying on its destructor");
    require(gFeeWriteWarnings.load() == QUIT_WRITE_FAILURES,
            "quit reports each injected failure queued before the event loop stops");
    FeePreferences last{.mode = FeeMode::Manual, .source = QStringLiteral("fee-quit-last")};
    requirePersisted(last);
    require(host->selectedFees().preferences == last,
            "shutdown persistence does not roll back the newest runtime selection");
    require(host->findChildren<QThread *>().empty(),
            "aboutToQuit joins and deletes its workers without deferred deletion");
    host->selectFees({.mode = FeeMode::Default, .source = QStringLiteral("fee-after-quit")});
    require(gFeeWriteRefusals.load() == 1, "the singleton explicitly refuses writes after quit");
    require(host->selectedFees().preferences == last,
            "a refused shutdown write does not change runtime fees");
    processFor(50);
    requirePersisted(last);
    require(gFeeWriteWarnings.load() == QUIT_WRITE_FAILURES && probe.outcomes().empty(),
            "stale queued completions after quit neither repeat errors nor publish callbacks");
}
} // namespace

FeeWriteProbe::FeeWriteProbe(HostImpl *host) {
    connect(host, &HostImpl::feePreferencesWritten, this, &FeeWriteProbe::onFeePreferencesWritten,
            qontrol::UNIQUE);
    connect(&m_timer, &QTimer::timeout, this, &FeeWriteProbe::onTick, qontrol::UNIQUE);
    m_timer.start(TICK_MS);
}

auto FeeWriteProbe::outcomes() const -> const QList<FeeWriteOutcome> & {
    return m_outcomes;
}

auto FeeWriteProbe::ticks() const -> int {
    return m_ticks;
}

void FeeWriteProbe::onFeePreferencesWritten(quint8 mode, const QString &source,
                                            const QString &error) {
    m_outcomes.append({.mode = mode, .source = source, .error = error});
}

void FeeWriteProbe::onTick() {
    ++m_ticks;
}

FeeReentrantSelection::FeeReentrantSelection(HostImpl *host) : m_host(host) {
    connect(host->events(), &HostEvents::selectedFeesChanged, this,
            &FeeReentrantSelection::onSelectedFeesChanged, qontrol::UNIQUE);
}

void FeeReentrantSelection::onSelectedFeesChanged() {
    if (m_reentered) {
        return;
    }
    m_reentered = true;
    m_host->selectFees({.mode = FeeMode::Smart, .source = QStringLiteral("fee-reentrant-newer")});
}

FeeQuitDriver::FeeQuitDriver(HostImpl *host) : m_host(host) {
}

void FeeQuitDriver::onQuitRequested() {
    m_host->selectFees({.mode = FeeMode::Default, .source = QStringLiteral("fee-quit-fails")});
    auto workers = m_host->findChildren<QThread *>();
    require(!workers.empty() && workers.last()->wait(WRITE_TIMEOUT_MS),
            "the first quit write finishes without processing its GUI completion");
    require(gFeeWriteWarnings.load() == 0,
            "the worker result is still queued when quit is requested");
#ifdef Q_OS_LINUX
    require(QDir().rmdir(feeTempPath()), "restoring persistence allows later shutdown writes");
#endif
    m_host->selectFees({.mode = FeeMode::Smart, .source = QStringLiteral("fee-quit-second")});
    m_host->selectFees({.mode = FeeMode::Manual, .source = QStringLiteral("fee-quit-last")});
    QCoreApplication::quit();
}

void feePersistenceSmoke() {
    settleSingletonWrites();
    fifoAndResponsiveGui();
    reentrantSelection();
    queuedSourceOwnsItsMemory();
    invalidModeRejected();
#ifdef Q_OS_LINUX
    failureAndRecovery();
#endif
    destructionDrains();
    singletonQuitDrains();
}

} // namespace smoke
