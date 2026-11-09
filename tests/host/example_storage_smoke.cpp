#include "example_storage_smoke.h"

#include "AppController.h"
#include "ExternalStorageConsumer.h"
#include "MockPlugin.h"
#include "PluginStopProbe.h"
#include "StorageProbe.h"
#include "host/Host.h"
#include "host/PluginMetadata.h"
#include "host/PluginRegistry.h"
#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <common.h>
#include <silent.h>
#include <string>

namespace smoke {
namespace {

const QString PLUGIN_ID = QStringLiteral("example");
const QString MODULE_ID = QStringLiteral("example.module");
const QString CREATE_ID = QStringLiteral("example.create");
const QString LEGACY_ID = QStringLiteral("example.legacy");
const QString LEGACY_NAME = QStringLiteral("Legacy account");
const QString STATE_PATH = QStringLiteral("example-state.json");
// The single state blob the seeding step sealed for the example plugin, byte for byte.
const QByteArray LEGACY_STATE =
    R"({"example.legacy":{"name":"Legacy account","settingEnabled":true}})";
const QByteArray EMPTY_STATE = "{}";
constexpr int TIMEOUT_MS = 60000;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("example storage smoke: %s", message);
    }
}

auto lists(const ExampleInstances &instances, const QString &id) -> bool {
    return std::ranges::any_of(instances, [&id](const auto &entry) { return entry.first == id; });
}

// ===== Host setup: physical files and Rust =====

// The sealed legacy blobs by file name. The opaque names and contents are only
// compared, never read.
auto legacyBlobs() -> QMap<QString, QByteArray> {
    QDir directory(QDir::home().filePath(QStringLiteral(".silent/plugin-state")));
    QMap<QString, QByteArray> blobs;
    for (const auto &name : directory.entryList(QDir::Files | QDir::Hidden)) {
        QFile file(directory.filePath(name));
        require(file.open(QIODevice::ReadOnly), "a legacy blob opens");
        blobs.insert(name, file.readAll());
    }
    return blobs;
}

auto sha256(const QByteArray &library) -> QString {
    return QString::fromLatin1(
        QCryptographicHash::hash(library, QCryptographicHash::Sha256).toHex());
}

void install(const QString &bundle) {
    auto installed = installPluginBundle(bundle);
    if (!installed.is_ok) {
        qFatal("example storage smoke: install failed: %s", installed.error.c_str());
    }
    require(QString::fromStdString(std::string(installed.plugin_id)) == PLUGIN_ID,
            "the installed bundle is the example plugin");
}

// The library the importer sealed, the bytes the host loads and grants by hash.
auto installedLibrary() -> QByteArray {
    auto opened = ::plugin_library(rust::String(PLUGIN_ID.toStdString()));
    require(opened.is_ok, "the installed example library opens");
    return {reinterpret_cast<const char *>(opened.library.data()),
            static_cast<qsizetype>(opened.library.size())};
}

// Answered before the plugin's first storage request, so the host asks nothing.
void allowSavedData(const QString &hash) {
    auto request = ::app_plugin_access_request(rust::String(PLUGIN_ID.toStdString()),
                                               rust::String(hash.toStdString()));
    require(request.is_ok, "the Saved Data request reads the app state");
    auto answers = request.access;
    answers.plugin_id = rust::String(PLUGIN_ID.toStdString());
    answers.hash = rust::String(hash.toStdString());
    answers.state = PluginGrant::Allowed;
    require(::app_set_plugin_access(answers, request.storage_epoch).empty(),
            "allow Saved Data before the first storage request");
}

// ===== Public module and storage APIs =====

auto exampleModule(PluginRegistry *registry) -> IModule * {
    for (auto *module : registry->enabledModulesForLauncher()) {
        if (module != nullptr && module->meta().value(QStringLiteral("id")) == MODULE_ID) {
            return module;
        }
    }
    qFatal("example storage smoke: the example module is not enabled");
}

void requireLoadedAbi(PluginRegistry *registry) {
    for (auto *plugin : registry->plugins()) {
        if (plugin->meta().value(QStringLiteral("id")) != PLUGIN_ID) {
            continue;
        }
        auto *object = dynamic_cast<QObject *>(plugin);
        require(object != nullptr && object->qt_metacast(SILENT_IPlugin_IID) != nullptr &&
                    object->qt_metacast("dev.silent.IPlugin/7.0") == nullptr &&
                    object->qt_metacast("dev.silent.IPlugin/8.0") == nullptr &&
                    object->qt_metacast("dev.silent.IPlugin/9.0") == nullptr &&
                    object->qt_metacast("dev.silent.IPlugin/10.0") == nullptr &&
                    object->qt_metacast("dev.silent.IPlugin/11.0") == nullptr &&
                    object->qt_metacast("dev.silent.IPlugin/13.0") == nullptr,
                "the loaded example plugin exposes ABI 12 only");
        return;
    }
    qFatal("example storage smoke: the example plugin is not loaded");
}

// The example's state file, read through the module's own storage.
auto readState(Host *host, IModule *module, const QString &label) -> QByteArray {
    ExternalStorageConsumer consumer(host, module, label);
    auto storage = consumer.open();
    auto answer = consumer.read(storage, STATE_PATH);
    require(ExternalStorageConsumer::error(answer).isEmpty(), "the example state file reads");
    auto data = ExternalStorageConsumer::data(answer);
    auto migration = consumer.migrate(storage, STATE_PATH);
    require(ExternalStorageConsumer::error(migration).isEmpty() &&
                !migration.value(QStringLiteral("migrated")).toBool() &&
                migration.value(QStringLiteral("reason")).toString() == QStringLiteral("completed"),
            "a completed migration is not repeated");
    require(ExternalStorageConsumer::data(consumer.read(storage, STATE_PATH)) == data,
            "the completed migration leaves the example state file");
    return data;
}

// The first answer comes before the storage is read; the load then refreshes the
// launcher with the migrated account.
void discoverLegacy(ExampleInstancesProbe &probe, PluginRegistry *registry, IModule *module) {
    auto first = probe.waitForAnswer(0);
    require(first.size() == 1 && first.first().first == CREATE_ID,
            "the first answer lists only the create entry: discovery does not wait for storage");
    auto loaded = probe.waitForAnswer(probe.waitForListing(LEGACY_ID, true, 1));
    require(loaded.contains(QPair<QString, QString>(LEGACY_ID, LEGACY_NAME)),
            "the migrated account keeps its name");
    require(lists(registry->launcherInstancesForModule(module), LEGACY_ID),
            "the launcher discovers the migrated account");
}

void requireMigratedState(Host *host, IModule *module) {
    auto data = readState(host, module, QStringLiteral("example-migrated"));
    require(data == LEGACY_STATE, "the migrated file holds the legacy blob byte for byte");
    auto accounts = QJsonDocument::fromJson(data).object();
    auto legacy = accounts.value(LEGACY_ID).toObject();
    require(accounts.size() == 1 &&
                legacy.value(QStringLiteral("name")).toString() == LEGACY_NAME &&
                legacy.value(QStringLiteral("settingEnabled")).toBool(),
            "the plugin's schema is kept: account names and settings");
}

// The launcher drops the account only once the module's write was confirmed.
void deleteLegacy(ExampleInstancesProbe &probe, PluginRegistry *registry, Host *host,
                  IModule *module) {
    auto before = probe.count();
    auto deletions = probe.deletionCount();
    registry->deleteInstance(module, LEGACY_ID);
    require(probe.deletionCount() == deletions, "accepted deletion waits for storage completion");
    require(probe.waitForDeletion(deletions).isEmpty(),
            "the public module reports deletion success");
    probe.waitForListing(LEGACY_ID, false, before);
    require(!lists(registry->launcherInstancesForModule(module), LEGACY_ID),
            "the launcher drops the deleted account");
    require(readState(host, module, QStringLiteral("example-deleted")) == EMPTY_STATE,
            "the confirmed write leaves the plugin's empty schema");
}

void deniedDeletion(ExampleInstancesProbe &probe, PluginRegistry *registry, Host *host,
                    IModule *module, const QString &hash) {
    auto request = ::app_plugin_access_request(rust::String(PLUGIN_ID.toStdString()),
                                               rust::String(hash.toStdString()));
    require(request.is_ok, "the revocation request reads the app state");
    auto answers = request.access;
    answers.state = PluginGrant::Denied;
    require(::app_set_plugin_access(answers, request.storage_epoch).empty(),
            "the revocation is saved");
    require(::app_plugin_answers(rust::String(PLUGIN_ID.toStdString()),
                                 rust::String(hash.toStdString()))
                    .state == PluginGrant::Denied,
            "revoke Saved Data before explicit deletion");
    SignalSpy refresh;
    QObject::connect(host->events(), &HostEvents::launcherRefreshRequested, &refresh,
                     &SignalSpy::onSignal, qontrol::UNIQUE);
    auto before = probe.deletionCount();
    registry->deleteInstance(module, LEGACY_ID);
    require(!registry->setEnabled(PLUGIN_ID, false), "a pending deletion pins the plugin library");
    require(probe.waitForDeletion(before) == QStringLiteral("denied"),
            "external deletion carries the exact storage denial");
    QElapsedTimer timer;
    timer.start();
    while (registry->accountDeletionPending(LEGACY_ID) && timer.elapsed() < TIMEOUT_MS) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(!registry->accountDeletionPending(LEGACY_ID),
            "failed deletion releases its correlated request");
    require(refresh.count() == 0 && lists(registry->launcherInstancesForModule(module), LEGACY_ID),
            "failed external deletion never refreshes or removes the discovered account");
    allowSavedData(hash);
    requireMigratedState(host, module);
}

// A reloaded plugin reads the file again and finds no account: the kept legacy blob is
// not imported a second time.
void reload(ExampleInstancesProbe &probe, PluginRegistry *registry, Host *host) {
    require(stopPlugin(registry, PLUGIN_ID).isEmpty(), "the example plugin unloads");
    require(enablePluginAndWait(registry, PLUGIN_ID), "the example plugin loads again");
    auto *module = exampleModule(registry);
    // The answer before the load, then the one the completed load refreshes.
    require(!lists(probe.waitForAnswer(0), LEGACY_ID), "the reloaded plugin lists no account");
    require(!lists(probe.waitForAnswer(1), LEGACY_ID),
            "the reloaded plugin does not import the legacy blob again");
    require(readState(host, module, QStringLiteral("example-reloaded")) == EMPTY_STATE,
            "the reloaded plugin keeps the persisted empty schema");
}

} // namespace

void ExampleInstancesProbe::watchEnables(PluginRegistry *registry) {
    m_registry = registry;
    connect(registry, &PluginRegistry::pluginEnableFinished, this,
            &ExampleInstancesProbe::onPluginEnabled, qontrol::UNIQUE);
}

void ExampleInstancesProbe::onPluginEnabled(const QString &id, const QString &error) {
    if (id == PLUGIN_ID && error.isEmpty()) {
        attach(exampleModule(m_registry));
    }
}

void ExampleInstancesProbe::attach(IModule *module) {
    if (m_module != nullptr) {
        disconnect(m_module, &IModule::instances, this, &ExampleInstancesProbe::onInstances);
        disconnect(m_module, &IModule::accountDeleted, this,
                   &ExampleInstancesProbe::onAccountDeleted);
    }
    m_module = module;
    m_answers.clear();
    m_deletion_errors.clear();
    connect(module, &IModule::instances, this, &ExampleInstancesProbe::onInstances,
            qontrol::UNIQUE);
    connect(module, &IModule::accountDeleted, this, &ExampleInstancesProbe::onAccountDeleted,
            qontrol::UNIQUE);
}

auto ExampleInstancesProbe::count() const -> qsizetype {
    return m_answers.size();
}

auto ExampleInstancesProbe::waitForAnswer(qsizetype index) -> ExampleInstances {
    QElapsedTimer deadline;
    deadline.start();
    while (m_answers.size() <= index && deadline.elapsed() < TIMEOUT_MS) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(m_answers.size() > index, "the example module answers list()");
    return m_answers.at(index);
}

auto ExampleInstancesProbe::waitForListing(const QString &id, bool listed, qsizetype after)
    -> qsizetype {
    QElapsedTimer deadline;
    deadline.start();
    auto next = after;
    while (deadline.elapsed() < TIMEOUT_MS) {
        for (; next < m_answers.size(); ++next) {
            if (lists(m_answers.at(next), id) == listed) {
                return next;
            }
        }
        QApplication::processEvents();
        QThread::msleep(1);
    }
    qFatal("example storage smoke: the example module never answered the expected listing");
}

void ExampleInstancesProbe::onInstances([[maybe_unused]] ReqId req_id,
                                        const ExampleInstances &instances) {
    m_answers.append(instances);
}

auto ExampleInstancesProbe::deletionCount() const -> qsizetype {
    return m_deletion_errors.size();
}

auto ExampleInstancesProbe::waitForDeletion(qsizetype index) -> QString {
    QElapsedTimer timer;
    timer.start();
    while (m_deletion_errors.size() <= index && timer.elapsed() < TIMEOUT_MS) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(m_deletion_errors.size() > index, "public module deletion result arrives");
    return m_deletion_errors.at(index);
}

void ExampleInstancesProbe::onAccountDeleted([[maybe_unused]] ReqId req_id, const QString &id,
                                             const QString &error) {
    require(id == LEGACY_ID, "public deletion result preserves account identity");
    m_deletion_errors.append(error);
}

void exampleStorageSmoke(const QString &bundle) {
    auto *host = Host::get();
    auto *registry = AppController::get()->pluginRegistry();
    require(host != nullptr && registry != nullptr, "the host and its plugin registry exist");

    auto blobs = legacyBlobs();
    require(blobs.size() == 1, "the seeding step left the example plugin's legacy blob");
    install(bundle);
    auto library = installedLibrary();
    require(pluginMetadataError(library, QStringLiteral(SILENT_IPlugin_IID)).isEmpty(),
            "the sealed example library declares plugin ABI 12, read without loading it");
    auto hash = sha256(library);
    allowSavedData(hash);
    // Any prompt means the preloaded grant was missed: it is denied, and the load fails.
    StoragePrompts prompts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &prompts, &StoragePrompts::onTick, qontrol::UNIQUE);
    timer.start(10);

    ExampleInstancesProbe probe;
    probe.watchEnables(registry);
    rescanPluginsAndWait(registry);
    require(enablePluginAndWait(registry, PLUGIN_ID), "the example plugin loads");
    require(registry->pluginHash(PLUGIN_ID) == hash, "the example runs under its library hash");
    requireLoadedAbi(registry);
    auto *module = exampleModule(registry);
    require(HostImpl::caller(module).external && HostImpl::caller(module).pluginHash == hash,
            "the example module is an external caller with its library hash");
    require(!lists(probe.waitForAnswer(0), LEGACY_ID),
            "the first discovery answer does not wait for storage");

    discoverLegacy(probe, registry, module);
    requireMigratedState(host, module);
    require(legacyBlobs() == blobs, "the migration keeps the legacy blob as it was");
    module->deleteInstance(LEGACY_ID);
    requireMigratedState(host, module);
    require(stopPlugin(registry, PLUGIN_ID).isEmpty(),
            "ordinary plugin unload preserves account data");
    require(enablePluginAndWait(registry, PLUGIN_ID),
            "plugin reloads after ordinary runtime cleanup");
    module = exampleModule(registry);
    discoverLegacy(probe, registry, module);
    requireMigratedState(host, module);
    deniedDeletion(probe, registry, host, module, hash);
    deleteLegacy(probe, registry, host, module);
    reload(probe, registry, host);
    require(legacyBlobs() == blobs, "the legacy blob is never overwritten");

    timer.stop();
    require(stopPlugin(registry, PLUGIN_ID).isEmpty(), "the example plugin unloads at the end");
    require(stopPlugin(registry, PLUGIN_ID, true).isEmpty(), "the example plugin is removed");
}

void exampleReinstallSmoke(const QString &bundle) {
    auto *host = Host::get();
    auto *registry = AppController::get()->pluginRegistry();
    require(host != nullptr && registry != nullptr, "restarted host and registry exist");
    require(legacyBlobs().isEmpty(), "removal deleted the legacy migration source before restart");
    require(::app_plugin_history(rust::String(PLUGIN_ID.toStdString())).empty(),
            "removal cleared every hash-specific grant before restart");
    install(bundle);
    auto hash = sha256(installedLibrary());
    allowSavedData(hash);
    ExampleInstancesProbe probe;
    probe.watchEnables(registry);
    rescanPluginsAndWait(registry);
    require(enablePluginAndWait(registry, PLUGIN_ID), "reinstall enables in the restarted host");
    auto *module = exampleModule(registry);
    require(!lists(probe.waitForAnswer(0), LEGACY_ID),
            "reinstalled plugin starts without old account IDs");
    require(!lists(probe.waitForAnswer(1), LEGACY_ID),
            "completed storage load cannot resurrect legacy accounts");
    ExternalStorageConsumer consumer(host, module, QStringLiteral("reinstalled-blank"));
    auto storage = consumer.open();
    require(ExternalStorageConsumer::error(consumer.read(storage, STATE_PATH)) ==
                QStringLiteral("not_found"),
            "reinstall has no old state file after actual process restart");
    auto migration = consumer.migrate(storage, STATE_PATH);
    require(ExternalStorageConsumer::error(migration).isEmpty() &&
                migration.value(QStringLiteral("reason")).toString() == QStringLiteral("completed"),
            "reinstalled plugin cannot rerun old blob migration");
    require(ExternalStorageConsumer::error(consumer.read(storage, STATE_PATH)) ==
                QStringLiteral("not_found"),
            "migration cannot resurrect a state file after reinstall");
    require(stopPlugin(registry, PLUGIN_ID, true).isEmpty(), "reinstalled plugin removes cleanly");
}

} // namespace smoke
