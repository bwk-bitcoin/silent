#include "AppController.h"
#include "MainWindow.h"
#include "MockPlugin.h"
#include "PluginBundleFixture.h"
#include "PluginStopProbe.h"
#include "catalog/feedback/Spinner.h"
#include "catalog/inputs/Toggle.h"
#include "host/PluginRegistry.h"
#include "interfaces/host.h"
#include "views/modals/AppSettings.h"
#include "views/modals/appsettings/AccessFlow.h"
#include "views/modals/appsettings/DisableFlow.h"
#include "views/modals/appsettings/RemoveFlow.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <common.h>
#include <memory>
#include <silent.h>
#include <string>

namespace smoke {
namespace {
constexpr int TIMEOUT_MS = 10000;

class RemovalBuiltin final : public IPlugin {
public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("removal.builtin")}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {};
    }
};

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("plugin removal: %s", message);
    }
}

template <typename Predicate> void waitForCompletion(Predicate completed, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (!completed() && timer.elapsed() < TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(completed(), message);
}

void waitForRegistry(PluginRegistry *registry) {
    waitForCompletion([registry] { return !registry->pluginStateLoading(); },
                      "checked asynchronous plugin state read completes");
}

void waitForStop(PluginRegistry *registry, const QString &id) {
    waitForCompletion([registry, &id] { return !registry->pluginStopPending(id); },
                      "plugin stop checkpoint settles after its runtime completion");
    waitForRegistry(registry);
}

void waitForEnable(PluginRegistry *registry, const QString &id) {
    waitForCompletion(
        [registry, &id] {
            return !registry->pluginEnablePending(id) && !registry->pluginStopPending(id) &&
                   !registry->pluginStateLoading();
        },
        "asynchronous plugin enable and any compensating disable complete");
}

void waitForAccess(modal::AccessFlow *flow) {
    auto *busy = flow->findChild<catalog::Spinner *>();
    require(busy != nullptr, "actual access flow exposes its read/save loading state");
    waitForCompletion([busy] { return busy->isHidden(); },
                      "asynchronous Settings access read or save completes");
}

auto read(const QString &path) -> QByteArray {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void allowNativeStorage(const QString &id, const QByteArray &bytes) {
    auto hash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    auto request = ::app_plugin_access_request(rust::String(id.toStdString()),
                                               rust::String(hash.toStdString()));
    require(request.is_ok, "the native fixture grant request reads the app state");
    auto access = request.access;
    access.plugin_id = rust::String(id.toStdString());
    access.hash = rust::String(hash.toStdString());
    access.state = PluginGrant::Allowed;
    require(::app_set_plugin_access(access, request.storage_epoch).empty(),
            "the native fixture grant is saved");
    require(::app_plugin_grants(rust::String(hash.toStdString())).state,
            "native fixture has a real hash-specific Saved Data grant");
}

auto removalPending(const QString &id) -> bool {
    auto pending = ::app_removing_plugins();
    require(pending.is_ok, "pending removals are checked before interpreting entries");
    return std::ranges::any_of(pending.entries, [&id](const auto &entry) {
        return QString::fromStdString(std::string(entry.plugin_id)) == id;
    });
}

auto visibleToggle(modal::AppSettings *settings, const QString &id) -> catalog::Toggle * {
    for (auto *toggle : settings->findChildren<catalog::Toggle *>()) {
        if (toggle->isVisible() && toggle->property("pluginId").toString() == id) {
            return toggle;
        }
    }
    return nullptr;
}

void mouseClick(QAbstractButton *button) {
    require(button != nullptr, "mouse action target exists");
    auto position = QPointF(button->rect().center());
    auto global = QPointF(button->mapToGlobal(position.toPoint()));
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(button, &press);
    QCoreApplication::sendEvent(button, &release);
    QCoreApplication::processEvents();
}

class RemovalProbe final : public QObject {
    Q_OBJECT

public:
    bool finished = false;
    bool removed = false;
    QString error;

    void wait() const {
        QElapsedTimer timer;
        timer.start();
        while (!finished && timer.elapsed() < TIMEOUT_MS) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        require(finished, "asynchronous removal completes");
    }

public slots:

    void onFinished([[maybe_unused]] const QString &id, bool was_removed, const QString &failure) {
        finished = true;
        removed = was_removed;
        error = failure;
    }

signals:
    void releaseWorker();
};

auto storageRequest(quint64 session, const QByteArray &request) -> QJsonObject {
    auto result = ::storage_request(session, rust::String(request.toStdString()));
    return QJsonDocument::fromJson(QByteArray(result.data(), result.size())).object();
}

void removeAndWait(PluginRegistry *registry, RemovalProbe &probe, const QString &id) {
    probe.finished = false;
    probe.error.clear();
    registry->removePlugin(id);
    probe.wait();
    waitForStop(registry, id);
}

void checkPendingReadFailure(PluginRegistry *registry, modal::AppSettings *settings,
                             const QString &id, const QString &known_phase,
                             const QByteArray &bytes) {
    auto failed = ::app_removing_plugins();
    require(!failed.is_ok && !failed.error.empty(),
            "Rust pending-list error is explicit, never a successful empty list");
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    require(!registry->pluginRemovalReadError().isEmpty(), "registry exposes pending-list failure");
    require(registry->pluginStopError(id) == known_phase,
            "failed rescan retains the last known removal phase");
    require(registry->pluginMetadata(id).value(QStringLiteral("id")) == id,
            "failed rescan does not discard the known pending row");
    auto *notice = settings->findChild<QWidget *>(QStringLiteral("pendingRemovalReadError"));
    auto *retry = settings->findChild<QPushButton *>(QStringLiteral("retryPendingRemovals"));
    require(notice != nullptr && retry != nullptr && notice->isVisible() && retry->isVisible() &&
                retry->isEnabled(),
            "actual Settings shows pending-list failure and working retry");
    bool namedFailure = false;
    for (auto *label : notice->findChildren<QLabel *>()) {
        namedFailure |= label->text() == QStringLiteral("Could not read pending plugin removals");
    }
    require(namedFailure, "the error is shown as a failure, not no pending removals");
    require(!settings->acceptDrops(), "unreadable state disables bundle drops");
    for (auto *flow : settings->findChildren<modal::RemoveFlow *>()) {
        require(!flow->isVisible() && !flow->isEnabled(),
                "an open destructive confirmation is withdrawn when its journal cannot be read");
    }
    int blockedButtons = 0;
    for (auto *button : settings->findChildren<QAbstractButton *>()) {
        if (button->property("pluginId").toString() == id && button->isVisible()) {
            require(!button->isEnabled(), "all plugin-row controls fail closed");
            ++blockedButtons;
        }
    }
    require(blockedButtons > 0, "the retained plugin row really contains blocked controls");
    require(!registry->setEnabled(id, true), "registry enable cannot bypass the read failure");
    require(!::app_set_plugin_enabled(rust::String(id.toStdString()), true).empty(),
            "direct persisted enable also refuses unreadable removal state");
    require(!installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "direct import cannot bypass the read failure");
    PluginRegistry restarted;
    waitForRegistry(&restarted);
    require(!restarted.pluginRemovalReadError().isEmpty(),
            "a new registry reports unreadable state instead of claiming no pending removal");
}

auto visibleButton(QWidget *page, const QString &text) -> QPushButton * {
    for (auto *button : page->findChildren<QPushButton *>()) {
        if (button->isVisible() && button->text() == text) {
            return button;
        }
    }
    qFatal("required action is missing: %s", qPrintable(text));
}

auto stateGrantToggle(QWidget *page) -> QCheckBox * {
    for (auto *toggle : page->findChildren<QCheckBox *>()) {
        if (toggle->isVisible() &&
            toggle->property("accessGrant").toString() == QStringLiteral("state")) {
            return toggle;
        }
    }
    qFatal("Saved Data toggle is missing");
}

auto hasVisibleAccessFailure(QWidget *page, const QString &error = QStringLiteral("removed since"))
    -> bool {
    return std::ranges::any_of(page->findChildren<QLabel *>(), [&error](auto *label) {
        return label->isVisible() && label->text().contains(error);
    });
}

void staleSettingsGrant(modal::AppSettings *settings, PluginRegistry *registry, const QString &id,
                        const QByteArray &bytes, bool forget, const QString &shots) {
    auto hash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    allowNativeStorage(id, bytes);
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    visibleButton(settings, QStringLiteral("Access"))->click();
    auto *flow = settings->findChild<modal::AccessFlow *>();
    require(flow != nullptr && flow->isVisible(),
            "actual Settings access view captures its lifetime");
    waitForAccess(flow);
    require(stateGrantToggle(flow)->isChecked(), "the old view starts from its Allowed answer");
    require(::app_begin_plugin_removal(rust::String(id.toStdString())).empty() &&
                ::plugin_remove(rust::String(id.toStdString())).empty(),
            "real administrative removal advances the durable epoch while the view stays open");
    require(installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "same-hash reinstall clears neither the epoch nor the stale-view fence");
    if (forget) {
        allowNativeStorage(id, bytes);
        visibleButton(flow, QStringLiteral("Forget"))->click();
    } else {
        mouseClick(stateGrantToggle(flow));
    }
    waitForAccess(flow);
    require(hasVisibleAccessFailure(flow), "stale Settings save is visibly rejected with retry");
    if (!shots.isEmpty()) {
        require(QDir().mkpath(shots), "create stale access captures directory");
        auto name = forget ? QStringLiteral("issue71-access-stale-forget-wip.png")
                           : QStringLiteral("issue71-access-stale-toggle-wip.png");
        require(settings->window()->grab().save(QDir(shots).filePath(name)),
                "capture the real whole window with the stale access failure");
    }
    require(!stateGrantToggle(flow)->isEnabled() && stateGrantToggle(flow)->isChecked(),
            "failed save leaves the old answer intact and disables stale controls");
    auto history = ::app_plugin_history(rust::String(id.toStdString()));
    require(forget ? history.size() == 1 : history.empty(),
            "stale toggle never recreates old grants; stale Forget never erases a fresh grant");
    ::state_lock();
    visibleButton(flow, QStringLiteral("Try Again"))->click();
    waitForAccess(flow);
    require(hasVisibleAccessFailure(flow, QStringLiteral("locked")) &&
                !stateGrantToggle(flow)->isEnabled() && stateGrantToggle(flow)->isChecked(),
            "a failed reload reports the read error and retains the disabled old rows");
    require(::state_unlock_with_password(rust::String("host smoke")).empty(),
            "unlock the production grant state before deliberate retry");
    if (!forget) {
        allowNativeStorage(id, bytes);
    }
    visibleButton(flow, QStringLiteral("Try Again"))->click();
    waitForAccess(flow);
    require(!hasVisibleAccessFailure(flow) && stateGrantToggle(flow)->isEnabled(),
            "deliberate reload captures the current installation lifetime");
    mouseClick(stateGrantToggle(flow));
    waitForAccess(flow);
    auto request = ::app_plugin_access_request(rust::String(id.toStdString()),
                                               rust::String(hash.toStdString()));
    require(request.is_ok && request.access.state == PluginGrant::Denied,
            "a fresh Settings action really saves the new Denied answer");
    visibleButton(flow, QStringLiteral("Forget"))->click();
    waitForAccess(flow);
    require(::app_plugin_history(rust::String(id.toStdString())).empty(),
            "fresh Forget saves against the current epoch");
    visibleButton(flow, QStringLiteral("Done"))->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
} // namespace

void pluginRemovalSmoke() {
    auto unlockPath = QDir::homePath() + QStringLiteral("/.silent/unlock.key");
    auto unlockBytes = read(unlockPath);
    require(!unlockBytes.isEmpty(), "actual password-wrapped application unlock key exists");
    QTemporaryDir directory;
    require(directory.isValid(), "isolated native sentinel directory");
    auto sentinel = directory.filePath(QStringLiteral("native-events"));
    qputenv("SILENT_REMOVAL_SENTINEL", QFile::encodeName(sentinel));
    auto bytes = read(QStringLiteral(SILENT_REMOVAL_FIXTURE));
    require(!bytes.isEmpty(), "actual native fixture bytes");
    auto id = QStringLiteral("removal.fixture");
    AppController::get()->registerBuiltinPlugin(std::make_unique<RemovalBuiltin>());
    auto *registry = AppController::get()->pluginRegistry();
    waitForRegistry(registry);
    RemovalProbe probe;
    QObject::connect(registry, &PluginRegistry::pluginStopFinished, &probe,
                     &RemovalProbe::onFinished, qontrol::UNIQUE);
    removeAndWait(registry, probe, QStringLiteral("removal.builtin"));
    require(!probe.removed && !probe.error.isEmpty() &&
                registry->isPluginBuiltin(QStringLiteral("removal.builtin")),
            "built-in removal is refused without removing the registration");

    auto installed = installBundle(id, QStringLiteral("Removal fixture"), bytes);
    require(installed.is_ok, "native bundle imports without execution");
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    require(read(sentinel).isEmpty(), "disabled inspection does not run a constructor");
    removeAndWait(registry, probe, id);
    require(probe.removed && probe.error.isEmpty(), "disabled plugin removal succeeds");
    require(read(sentinel).isEmpty(), "disabled removal never loads the library");

    require(installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "reinstall empty plugin");
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    auto session = ::storage_session_open(rust::String(id.toStdString()), rust::String(), false);
    auto opened = storageRequest(session, R"({"id":"open","op":"open","storage":"private"})");
    auto handle = opened.value(QStringLiteral("storage")).toString();
    QJsonObject write{{QStringLiteral("id"), QStringLiteral("write")},
                      {QStringLiteral("op"), QStringLiteral("write")},
                      {QStringLiteral("storage"), handle},
                      {QStringLiteral("path"), QStringLiteral("account")},
                      {QStringLiteral("data"), QStringLiteral("d2FsbGV0")},
                      {QStringLiteral("mode"), QStringLiteral("create")}};
    require(storageRequest(session, QJsonDocument(write).toJson())
                .value(QStringLiteral("error"))
                .toString()
                .isEmpty(),
            "seed real private account file");
    allowNativeStorage(id, bytes);
    require(registry->setEnabled(id, true), "enable real native worker");
    waitForEnable(registry, id);
    require(registry->isPluginEnabled(id), "real native worker finishes enabling");
    auto privatePath = QDir::homePath() + QStringLiteral("/.silent/removal.fixture/tree.bip138");
    auto privateBytes = read(privatePath);
    require(!privateBytes.isEmpty(), "native fixture has actual encrypted private data");
    IModule *nativeModule = nullptr;
    for (auto *module : registry->enabledModulesForLauncher()) {
        if (module->meta().value(QStringLiteral("id")) == QStringLiteral("removal.module")) {
            nativeModule = module;
        }
    }
    require(nativeModule != nullptr, "native module owns the delayed worker");
    QObject::connect(&probe, SIGNAL(releaseWorker()), nativeModule, SLOT(releaseWorker()),
                     qontrol::UNIQUE);
    qputenv("SILENT_REMOVAL_REFUSE", "1");
    removeAndWait(registry, probe, id);
    require(probe.error.contains(QStringLiteral("deliberate native refusal")),
            "native shutdown failure is explicit");
    require(!registry->isPluginEnabled(id), "failed removal stays disabled");
    require(!read(sentinel).contains("unload"), "refused shutdown cannot unload the library");
    require(read(privatePath) == privateBytes,
            "shutdown refusal preserves complete private ciphertext");
    require(!registry->setEnabled(id, true), "pending removal cannot be enabled");
    require(Host::get()->keys(nativeModule) == nullptr &&
                Host::get()->transactions(nativeModule) == nullptr &&
                Host::get()->instances(nativeModule).isEmpty(),
            "closing admission refuses all new capability handles");
    registry->deleteInstance(nativeModule, QStringLiteral("removal.account"));
    require(nativeModule->property("deleteCalls").toInt() == 0,
            "account deletion cannot start after module shutdown admission closes");
    require(Host::get()->storage(nativeModule) == nullptr,
            "closing admission refuses new storage acquisition while retaining the owned handle "
            "for shutdown persistence");
    require(!installBundle(id, QStringLiteral("replacement"), bytes).is_ok,
            "pending removal cannot be bypassed by reinstall");
    qunsetenv("SILENT_REMOVAL_REFUSE");
    probe.finished = false;
    registry->removePlugin(id);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 50) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    require(!probe.finished, "native work remains pending after runtime registrations are gone");
    require(!read(sentinel).contains("unload"), "library remains mapped during native delay");
    auto writeFailure = qEnvironmentVariableIsSet("SILENT_EXPECT_NATIVE_WRITE_FAILURE");
    auto privatePermissions = QFileInfo(QFileInfo(privatePath).path()).permissions();
    if (writeFailure) {
        require(QFile::setPermissions(QFileInfo(privatePath).path(),
                                      QFile::ReadOwner | QFile::ExeOwner),
                "make native final persistence fail with actual EACCES");
    }
    emit probe.releaseWorker();
    probe.wait();
    waitForStop(registry, id);
    if (writeFailure) {
        require(QFile::setPermissions(QFileInfo(privatePath).path(), privatePermissions),
                "restore private storage permissions");
        require(probe.error.contains(QStringLiteral("io")),
                "native final persistence failure is explicit and cannot permit unload");
        require(!read(sentinel).contains("unload"),
                "failed native flush retains the mapped worker owner");
        require(read(privatePath) == privateBytes,
                "failed final persistence leaves the original complete private tree");
        removeAndWait(registry, probe, id);
    }
    if (qEnvironmentVariableIsSet("SILENT_EXPECT_UNLOAD_FAILURE")) {
        require(probe.error.contains(QStringLiteral("Unload failed")),
                "actual dlclose failure is explicit");
        require(!read(sentinel).contains("unload"),
                "failed dlclose keeps the native library mapped");
        QJsonObject retained{{QStringLiteral("id"), QStringLiteral("retained")},
                             {QStringLiteral("op"), QStringLiteral("read")},
                             {QStringLiteral("storage"), handle},
                             {QStringLiteral("path"), QStringLiteral("account")}};
        require(storageRequest(session, QJsonDocument(retained).toJson())
                        .value(QStringLiteral("data")) == QStringLiteral("d2FsbGV0"),
                "unload failure preserves the original account alongside completed final writes");
        retained[QStringLiteral("path")] = QStringLiteral("shutdown-flush");
        require(storageRequest(session, QJsonDocument(retained).toJson())
                        .value(QStringLiteral("data")) ==
                    QStringLiteral("bmF0aXZlLWZpbmFsLXdyaXRl"),
                "native shutdown final persistence completes before the unload attempt");
        require(::plugin_library(rust::String(id.toStdString())).is_ok,
                "unload failure retains bundle and metadata");
        require(removalPending(id), "unload failure remains disabled and retryable");
        removeAndWait(registry, probe, id);
    }
    require(probe.removed && probe.error.isEmpty(),
            "retry waits for native completion then removes");
    auto events = read(sentinel);
    require(
        events.count("widget\n") == 1 && events.indexOf("widget\n") < events.indexOf("unload\n"),
        "native widget destruction precedes library unload without a global deferred-delete flush");
    events.replace("widget\n", "");
    require(events == (writeFailure ? "constructor\nworker\nflush-failed\nflush\nunload\n"
                                    : "constructor\nworker\nflush\nunload\n"),
            "worker completion precedes real library-unload sentinel");
    require(!removalPending(id), "successful cleanup clears durable journal");
    require(installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "reinstall after successful purge");
    auto restarted = ::storage_session_open(rust::String(id.toStdString()), rust::String(), false);
    opened = storageRequest(restarted, R"({"id":"open","op":"open","storage":"private"})");
    QJsonObject inspect{{QStringLiteral("id"), QStringLiteral("inspect")},
                        {QStringLiteral("op"), QStringLiteral("inspect")},
                        {QStringLiteral("storage"), opened.value(QStringLiteral("storage"))},
                        {QStringLiteral("path"), QStringLiteral("account")}};
    require(
        storageRequest(restarted, QJsonDocument(inspect).toJson()).value(QStringLiteral("error")) ==
            QStringLiteral("not_found"),
        "reinstall starts without old account data");
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    auto bundlesDirectory = QDir::homePath() + QStringLiteral("/.silent/plugins");
    auto permissions = QFileInfo(bundlesDirectory).permissions();
    require(QFile::setPermissions(bundlesDirectory, QFile::ReadOwner | QFile::ExeOwner),
            "make installed bundle directory unwritable");
    removeAndWait(registry, probe, id);
    require(QFile::setPermissions(bundlesDirectory, permissions),
            "restore installed bundle directory permissions");
    require(probe.error.contains(QStringLiteral("bundle removal failed")),
            "bundle deletion failure reports the already-cleared private-data phase");
    require(removalPending(id), "failed bundle deletion retains retry journal");
    require(::plugin_library(rust::String(id.toStdString())).is_ok,
            "failed bundle deletion does not hide installed metadata");
    removeAndWait(registry, probe, id);
    require(probe.error.isEmpty(), "final disabled cleanup succeeds");

    auto oldLibrary = read(QStringLiteral(SILENT_ABI10_FIXTURE_PATH));
    auto compatibility = libraryCompatibility(oldLibrary);
    auto oldId = QStringLiteral("incompatible.fixture");
    auto payload = QStringLiteral("payload/libold.so");
    auto archive = storedZip(
        {{QStringLiteral("manifest.json"),
          bundleManifest(oldId, QStringLiteral("Old installed plugin"), payload, compatibility)},
         {payload, oldLibrary}});
    auto oldBundlePath = directory.filePath(QStringLiteral("old-bundle"));
    QFile oldBundle(oldBundlePath);
    require(oldBundle.open(QIODevice::WriteOnly) && oldBundle.write(archive) == archive.size(),
            "old installation fixture bundle");
    oldBundle.close();
    // Admit with the old host expectation to model an encrypted install surviving an upgrade.
    auto legacy = ::plugin_install(rust::String(oldBundlePath.toStdString()),
                                   rust::String("dev.silent.IPlugin/10.0"), QT_VERSION_MAJOR,
                                   QT_VERSION_MINOR, hostArchRequirements());
    require(legacy.is_ok, "old host admits old installation without executing it");
    auto oldSentinel = directory.filePath(QStringLiteral("old-constructor"));
    qputenv("SILENT_ABI_SENTINEL", QFile::encodeName(oldSentinel));
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    require(registry->setEnabled(oldId, true), "incompatible library enable check is admitted");
    waitForEnable(registry, oldId);
    require(!registry->isPluginEnabled(oldId),
            "current host refuses incompatible installed library");
    require(!registry->pluginLoadError(oldId).isEmpty(),
            "incompatible plugin keeps its explicit error");
    removeAndWait(registry, probe, oldId);
    require(probe.error.isEmpty(), "incompatible installed plugin removes through metadata");
    require(!QFile::exists(oldSentinel),
            "incompatible inspection, load refusal and removal never run its constructor");
    qunsetenv("SILENT_ABI_SENTINEL");
    require(read(unlockPath) == unlockBytes,
            "enabled, disabled, incompatible and retried removal preserve actual unlock-key bytes");
    qunsetenv("SILENT_REMOVAL_SENTINEL");
}

void pluginAccessSettingsFailureSmoke(const QString &shots) {
    auto bytes = read(QStringLiteral(SILENT_REMOVAL_FIXTURE));
    auto id = QStringLiteral("removal.fixture");
    require(installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "access failure fixture installs without loading native code");
    auto *controller = AppController::get();
    controller->registerBuiltinPlugin(std::make_unique<MockPlugin>());
    auto *registry = controller->pluginRegistry();
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    MainWindow window;
    controller->start(&window);
    window.resize(1440, 1000);
    window.show();
    auto *settings = window.findChild<modal::AppSettings *>();
    require(settings != nullptr, "actual Settings exists for grant lifetime exercise");
    window.showTab(settings);
    QCoreApplication::processEvents();
    waitForRegistry(registry);
    staleSettingsGrant(settings, registry, id, bytes, false, shots);
    staleSettingsGrant(settings, registry, id, bytes, true, shots);
}

void pluginRemovalReadFailureSmoke(const QString &shots) {
    auto bytes = read(QStringLiteral(SILENT_REMOVAL_FIXTURE));
    auto id = QStringLiteral("removal.fixture");
    require(installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "read-failure fixture installs");
    require(::app_begin_plugin_removal(rust::String(id.toStdString())).empty(),
            "read-failure fixture has a durable pending journal");
    auto *controller = AppController::get();
    controller->registerBuiltinPlugin(std::make_unique<MockPlugin>());
    auto *registry = controller->pluginRegistry();
    registry->rescanDiscoveredPlugins();
    waitForRegistry(registry);
    auto knownPhase = registry->pluginStopError(id);
    require(!knownPhase.isEmpty(), "registry initially knows the pending phase");
    MainWindow window;
    controller->start(&window);
    window.resize(1440, 1000);
    window.show();
    auto *settings = window.findChild<modal::AppSettings *>();
    require(settings != nullptr, "real Settings exists for read-failure exercise");
    window.showTab(settings);
    QCoreApplication::processEvents();
    waitForRegistry(registry);
    auto statePath = QDir::homePath() + QStringLiteral("/.silent/app.bip138");
    auto lockPath = QDir::homePath() + QStringLiteral("/.silent/app.lock");
    auto original = read(statePath);
    auto statePermissions = QFileInfo(statePath).permissions();
    auto lockPermissions = QFileInfo(lockPath).permissions();
    require(!original.isEmpty(), "read-failure journal contains real ciphertext");
    auto *notice = settings->findChild<QWidget *>(QStringLiteral("pendingRemovalReadError"));
    auto *retry = settings->findChild<QPushButton *>(QStringLiteral("retryPendingRemovals"));
    require(notice != nullptr && retry != nullptr, "named read failure and retry controls exist");
    RemovalProbe probe;
    QObject::connect(registry, &PluginRegistry::pluginStopFinished, &probe,
                     &RemovalProbe::onFinished, qontrol::UNIQUE);
    for (const auto &fault : {QStringLiteral("locked"), QStringLiteral("corrupt"),
                              QStringLiteral("unreadable"), QStringLiteral("unwritable-lock")}) {
        visibleButton(settings, QStringLiteral("Remove"))->click();
        auto confirmations = settings->findChildren<modal::RemoveFlow *>();
        QPointer<modal::RemoveFlow> confirmation;
        for (auto *candidate : confirmations) {
            if (candidate->isVisible()) {
                confirmation = candidate;
            }
        }
        require(confirmation != nullptr,
                "read failure also protects a confirmation that was already open");
        if (fault == QStringLiteral("locked")) {
            ::state_lock();
        } else if (fault == QStringLiteral("corrupt")) {
            QFile state(statePath);
            require(state.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                        state.write("corrupt journal") > 0,
                    "install corrupt journal fixture");
        } else {
            require(
                QFile::setPermissions(fault == QStringLiteral("unreadable") ? statePath : lockPath,
                                      fault == QStringLiteral("unreadable") ? QFile::Permissions()
                                                                            : QFile::ReadOwner),
                "install actual inaccessible journal or unwritable lock fixture");
        }
        checkPendingReadFailure(registry, settings, id, knownPhase, bytes);
        require(confirmation == nullptr ||
                    (!confirmation->isVisible() && !confirmation->isEnabled()),
                "the actual previously open confirmation is gone or withdrawn and disabled");
        auto error = registry->pluginRemovalReadError();
        removeAndWait(registry, probe, id);
        require(!probe.removed && probe.error == error,
                "removal reports the read failure instead of destructive success");
        retry->click();
        waitForRegistry(registry);
        require(!registry->pluginRemovalReadError().isEmpty() && notice->isVisible(),
                "retry while still inaccessible remains an explicit failure");
        if (!shots.isEmpty()) {
            require(QDir().mkpath(shots), "create read-failure captures directory");
            require(window.grab().save(QDir(shots).filePath(
                        QStringLiteral("issue71-pending-%1-wip.png").arg(fault))),
                    "capture real whole-window pending-read failure");
        }
        if (fault == QStringLiteral("locked")) {
            require(::state_unlock_with_password(rust::String("host smoke")).empty(),
                    "unlock production state for retry without any device");
        } else if (fault == QStringLiteral("corrupt")) {
            QFile state(statePath);
            require(state.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                        state.write(original) == original.size(),
                    "restore exact original ciphertext");
        } else {
            require(QFile::setPermissions(
                        fault == QStringLiteral("unreadable") ? statePath : lockPath,
                        fault == QStringLiteral("unreadable") ? statePermissions : lockPermissions),
                    "restore file access before retry");
        }
        require(read(statePath) == original,
                "failed reads and refused operations preserve journal bytes");
        retry->click();
        waitForRegistry(registry);
        require(registry->pluginRemovalReadError().isEmpty() && !notice->isVisible() &&
                    !retry->isVisible() && settings->acceptDrops(),
                "successful checked retry restores controls and clears only the error");
        require(removalPending(id) && registry->pluginStopError(id) == knownPhase,
                "successful read recovers the real pending record, not an empty default");
        require(!registry->setEnabled(id, true),
                "recovered pending removal still cannot be enabled");
    }
    removeAndWait(registry, probe, id);
    require(probe.removed && probe.error.isEmpty() && !removalPending(id),
            "recovered journal permits actual metadata-only cleanup to finish");
}

void pluginRemovalUiSmoke(const QString &shots) {
    auto bytes = read(QStringLiteral(SILENT_REMOVAL_FIXTURE));
    auto id = QStringLiteral("removal.fixture");
    require(installBundle(id, QStringLiteral("Removal fixture"), bytes).is_ok,
            "UI fixture installs");
    auto *controller = AppController::get();
    controller->registerBuiltinPlugin(std::make_unique<MockPlugin>());
    auto *registry = controller->pluginRegistry();
    MainWindow window;
    controller->start(&window);
    window.resize(1440, 1000);
    window.show();
    auto *settings = window.findChild<modal::AppSettings *>();
    require(settings != nullptr, "actual MainWindow owns Settings");
    window.showTab(settings);
    QCoreApplication::processEvents();
    waitForRegistry(registry);
    ::app_set_active_theme(rust::String("light"));
    auto appState = QDir::homePath() + QStringLiteral("/.silent/app.bip138");
    auto original = read(appState);
    require(!original.isEmpty(), "cancellation checks nonempty encrypted application state");
    auto capture = [&window, &shots](const QString &name) {
        if (shots.isEmpty()) {
            return;
        }
        require(QDir().mkpath(shots), "create whole-window capture directory");
        QCoreApplication::processEvents();
        require(window.grab().save(QDir(shots).filePath(name)), "save actual whole MainWindow");
    };
    auto removeButton = [settings, &id]() -> QPushButton * {
        for (auto *button : settings->findChildren<QPushButton *>()) {
            if (button->isVisible() && button->property("pluginId").toString() == id &&
                button->text() == QStringLiteral("Remove")) {
                return button;
            }
        }
        return nullptr;
    };
    auto click = [](QWidget *page, const QString &text) {
        for (auto *button : page->findChildren<QPushButton *>()) {
            if (button->isVisible() && button->text() == text) {
                button->click();
                return;
            }
        }
        qFatal("removal UI action is missing: %s", qPrintable(text));
    };
    require(removeButton() != nullptr && removeButton()->isVisible(),
            "external plugin row offers visible Remove");
    capture(QStringLiteral("issue71-settings-wip.png"));
    removeButton()->click();
    QCoreApplication::processEvents();
    auto *flow = settings->findChild<modal::RemoveFlow *>();
    require(flow != nullptr && flow->isVisible(),
            "Remove opens the actual destructive confirmation flow");
    bool named = false;
    bool warning = false;
    for (auto *label : flow->findChildren<QLabel *>()) {
        named |= label->text() == QStringLiteral("Remove Removal fixture?");
        warning |= label->text().contains(QStringLiteral("all of its saved state and accounts"));
    }
    require(named && warning,
            "confirmation names the plugin and warns about deletion of all state/accounts");
    capture(QStringLiteral("issue71-confirmation-wip.png"));
    click(flow, QStringLiteral("Back"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(read(appState) == original,
            "confirmation cancellation leaves app-state ciphertext byte-identical");
    require(!removalPending(id), "cancel never prepares removal");
    auto library = ::plugin_library(rust::String(id.toStdString()));
    require(library.is_ok && QByteArray(reinterpret_cast<const char *>(library.library.data()),
                                        library.library.size()) == bytes,
            "cancel preserves the installed native bytes");
    allowNativeStorage(id, bytes);
    auto *toggle = visibleToggle(settings, id);
    require(toggle != nullptr && !toggle->isChecked(), "real external toggle starts disabled");
    mouseClick(toggle);
    waitForEnable(registry, id);
    require(registry->isPluginEnabled(id),
            "actual mouse events enable the plugin through its toggle");
    RemovalProbe native;
    for (auto *module : registry->enabledModulesForLauncher()) {
        if (module->meta().value(QStringLiteral("id")) == QStringLiteral("removal.module")) {
            QObject::connect(&native, SIGNAL(releaseWorker()), module, SLOT(releaseWorker()),
                             qontrol::UNIQUE);
            emit native.releaseWorker();
        }
    }
    qputenv("SILENT_REMOVAL_REFUSE", "1");
    mouseClick(visibleToggle(settings, id));
    auto *disableFlow = settings->findChild<modal::DisableFlow *>();
    require(disableFlow != nullptr, "real disable confirmation opens");
    PluginStopProbe disabledFailure(registry, id);
    click(disableFlow, QStringLiteral("Disable Plugin"));
    require(disabledFailure.wait().contains(QStringLiteral("deliberate native refusal")),
            "failed disable shows the actual native error");
    waitForStop(registry, id);
    qunsetenv("SILENT_REMOVAL_REFUSE");
    require(registry->pluginDisableRetryable(id) && !removalPending(id),
            "failed disable has a non-destructive retry, not a removal journal");
    capture(QStringLiteral("issue71-disable-failed-wip.png"));
    PluginStopProbe retriedDisable(registry, id);
    click(settings, QStringLiteral("Retry disable"));
    require(retriedDisable.wait().isEmpty(),
            "retry disable completes without selecting destructive Remove");
    waitForStop(registry, id);
    require(::plugin_library(rust::String(id.toStdString())).is_ok,
            "disable retry retains installed bundle");
    auto preservedSession =
        ::storage_session_open(rust::String(id.toStdString()), rust::String(), false);
    auto preserved =
        storageRequest(preservedSession, R"({"id":"open","op":"open","storage":"private"})");
    QJsonObject saved{{QStringLiteral("id"), QStringLiteral("saved")},
                      {QStringLiteral("op"), QStringLiteral("read")},
                      {QStringLiteral("storage"), preserved.value(QStringLiteral("storage"))},
                      {QStringLiteral("path"), QStringLiteral("shutdown-flush")}};
    require(storageRequest(preservedSession, QJsonDocument(saved).toJson())
                    .value(QStringLiteral("data")) == QStringLiteral("bmF0aXZlLWZpbmFsLXdyaXRl"),
            "retry disable preserves actual final backend data");
    ::storage_session_revoke(preservedSession);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    mouseClick(visibleToggle(settings, id));
    waitForEnable(registry, id);
    require(registry->isPluginEnabled(id), "a safely disabled plugin can be enabled again");
    for (auto *module : registry->enabledModulesForLauncher()) {
        if (module->meta().value(QStringLiteral("id")) == QStringLiteral("removal.module")) {
            QObject::connect(&native, SIGNAL(releaseWorker()), module, SLOT(releaseWorker()),
                             qontrol::UNIQUE);
            emit native.releaseWorker();
        }
    }
    capture(QStringLiteral("issue71-enabled-wip.png"));
    require(!removeButton()->isEnabled(), "Remove is disabled while the plugin is enabled");
    mouseClick(visibleToggle(settings, id));
    disableFlow = settings->findChild<modal::DisableFlow *>();
    require(disableFlow != nullptr, "disabling before removal opens its confirmation");
    PluginStopProbe disabledFirst(registry, id);
    click(disableFlow, QStringLiteral("Disable Plugin"));
    require(disabledFirst.wait().isEmpty(), "the plugin disables before removal");
    waitForStop(registry, id);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!registry->isPluginEnabled(id) && removeButton()->isEnabled(),
            "Remove is enabled once the plugin is disabled");
    capture(QStringLiteral("issue80-disabled-wip.png"));
    removeButton()->click();
    QCoreApplication::processEvents();
    flow = settings->findChild<modal::RemoveFlow *>();
    require(flow != nullptr, "confirmation can be reopened after cancellation");
    PluginStopProbe completion(registry, id);
    click(flow, QStringLiteral("Remove"));
    auto error = completion.wait();
    require(error.isEmpty(), "real Settings removal completes successfully");
    waitForStop(registry, id);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(removeButton() == nullptr, "completion refresh removes the installed row");
    capture(QStringLiteral("issue71-removed-wip.png"));
}

} // namespace smoke

#include "plugin_removal_smoke.moc"
