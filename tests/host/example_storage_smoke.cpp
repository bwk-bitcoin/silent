#include "example_storage_smoke.h"

#include "AppController.h"
#include "ExternalStorageConsumer.h"
#include "StorageProbe.h"
#include "host/Host.h"
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
#include <QPluginLoader>
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

auto sha256(const QString &path) -> QString {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "the example library opens");
    return QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

// The library declares the current plugin ABI, read without loading it.
void requireBinaryAbi(const QString &library) {
    QPluginLoader loader(library);
    require(loader.metaData().value(QStringLiteral("IID")).toString() ==
                QLatin1String(SILENT_IPlugin_IID),
            "the example library declares plugin ABI 7");
}

void install(const QString &directory) {
    auto installed = ::plugin_install(rust::String(directory.toStdString()));
    if (!installed.is_ok) {
        qFatal("example storage smoke: install failed: %s", installed.error.c_str());
    }
    require(QString::fromStdString(std::string(installed.plugin_id)) == PLUGIN_ID,
            "the installed bundle is the example plugin");
}

// Answered before the plugin's first storage request, so the host asks nothing.
void allowSavedData(const QString &hash) {
    auto answers = ::app_plugin_answers(rust::String(PLUGIN_ID.toStdString()),
                                        rust::String(hash.toStdString()));
    answers.plugin_id = rust::String(PLUGIN_ID.toStdString());
    answers.hash = rust::String(hash.toStdString());
    answers.state = PluginGrant::Allowed;
    ::app_set_plugin_access(answers);
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
                    object->qt_metacast("dev.silent.IPlugin/6.0") == nullptr,
                "the loaded example plugin exposes ABI 7 only");
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
    module->deleteInstance(LEGACY_ID);
    probe.waitForListing(LEGACY_ID, false, before);
    require(!lists(registry->launcherInstancesForModule(module), LEGACY_ID),
            "the launcher drops the deleted account");
    require(readState(host, module, QStringLiteral("example-deleted")) == EMPTY_STATE,
            "the confirmed write leaves the plugin's empty schema");
}

// A reloaded plugin reads the file again and finds no account: the kept legacy blob is
// not imported a second time.
void reload(ExampleInstancesProbe &probe, PluginRegistry *registry, Host *host) {
    require(registry->setEnabled(PLUGIN_ID, false), "the example plugin unloads");
    require(registry->setEnabled(PLUGIN_ID, true), "the example plugin loads again");
    auto *module = exampleModule(registry);
    probe.attach(module);
    // The answer before the load, then the one the completed load refreshes.
    require(!lists(probe.waitForAnswer(0), LEGACY_ID), "the reloaded plugin lists no account");
    require(!lists(probe.waitForAnswer(1), LEGACY_ID),
            "the reloaded plugin does not import the legacy blob again");
    require(readState(host, module, QStringLiteral("example-reloaded")) == EMPTY_STATE,
            "the reloaded plugin keeps the persisted empty schema");
}

} // namespace

void ExampleInstancesProbe::attach(IModule *module) {
    if (m_module != nullptr) {
        disconnect(m_module, &IModule::instances, this, &ExampleInstancesProbe::onInstances);
    }
    m_module = module;
    m_answers.clear();
    connect(module, &IModule::instances, this, &ExampleInstancesProbe::onInstances,
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

void exampleStorageSmoke(const QString &directory) {
    auto *host = Host::get();
    auto *registry = AppController::get()->pluginRegistry();
    require(host != nullptr && registry != nullptr, "the host and its plugin registry exist");

    auto blobs = legacyBlobs();
    require(blobs.size() == 1, "the seeding step left the example plugin's legacy blob");
    auto library = QDir(directory).filePath(QStringLiteral("libexample.so"));
    requireBinaryAbi(library);
    auto hash = sha256(library);
    install(directory);
    allowSavedData(hash);
    // Any prompt means the preloaded grant was missed: it is denied, and the load fails.
    StoragePrompts prompts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &prompts, &StoragePrompts::onTick, qontrol::UNIQUE);
    timer.start(10);

    registry->rescanDiscoveredPlugins();
    require(registry->setEnabled(PLUGIN_ID, true), "the example plugin loads");
    require(registry->pluginHash(PLUGIN_ID) == hash, "the example runs under its library hash");
    requireLoadedAbi(registry);
    auto *module = exampleModule(registry);
    require(HostImpl::caller(module).external && HostImpl::caller(module).pluginHash == hash,
            "the example module is an external caller with its library hash");
    ExampleInstancesProbe probe;
    probe.attach(module);
    require(!lists(registry->launcherInstancesForModule(module), LEGACY_ID),
            "enabling the plugin does not read its storage synchronously");

    discoverLegacy(probe, registry, module);
    requireMigratedState(host, module);
    require(legacyBlobs() == blobs, "the migration keeps the legacy blob as it was");
    deleteLegacy(probe, registry, host, module);
    reload(probe, registry, host);
    require(legacyBlobs() == blobs, "the legacy blob is never overwritten");

    timer.stop();
    require(registry->setEnabled(PLUGIN_ID, false), "the example plugin unloads at the end");
    require(std::string(::plugin_remove(rust::String(PLUGIN_ID.toStdString()))).empty(),
            "the example plugin is removed");
}

} // namespace smoke
