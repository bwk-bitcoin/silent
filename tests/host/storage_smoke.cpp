#include "AppController.h"
#include "ExternalStorageConsumer.h"
#include "MockPlugin.h"
#include "StorageProbe.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUuid>
#include <memory>
#include <silent.h>
#include <utility>

// Synchronize worker completion without delivering its queued result to the public consumer.
#define private public
#include "host/StorageHandle.h"
#undef private

#ifdef SILENT_STORAGE_FIXTURE
#include "PluginBundleFixture.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QVariant>
#include <string>
#endif

namespace smoke {
namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("storage smoke: %s", message);
    }
}

auto error(const QJsonObject &answer) -> QString {
    return ExternalStorageConsumer::error(answer);
}

void requireError(const QJsonObject &answer, const char *code, const char *message) {
    require(error(answer) == QLatin1String(code), message);
}

void requireDone(const QJsonObject &answer, const char *message) {
    require(error(answer).isEmpty(), message);
}

void finishWorker(ExternalStorageConsumer &consumer) {
    auto *handle = dynamic_cast<host::StorageHandle *>(consumer.storage());
    require(handle != nullptr && handle->m_job != nullptr, "a public request started its worker");
    require(handle->m_job->wait(10000), "the worker computed its answer before GUI delivery");
}

auto writeRequest(const QString &storage, const QString &path, const QByteArray &data)
    -> QJsonObject {
    return {{QStringLiteral("op"), QStringLiteral("write")},
            {QStringLiteral("storage"), storage},
            {QStringLiteral("path"), path},
            {QStringLiteral("data"), QString::fromLatin1(data.toBase64())},
            {QStringLiteral("mode"), QStringLiteral("upsert")}};
}

// A mutation from another session. Operations of every session take turns, and a
// revoked or cancelled request re-checks itself once it has the turn: once this one is
// answered, every request revoked before it was sent has published all it ever will.
void settle(ExternalStorageConsumer &witness, const QString &storage) {
    requireDone(
        witness.write(storage, QStringLiteral("barrier.txt"), "barrier", QStringLiteral("upsert")),
        "the witness mutation is answered");
}

// A write cut short while it ran left the file absent or whole, never partial.
void requireWholeOrAbsent(const QJsonObject &answer, const QByteArray &expected,
                          const char *message) {
    require(error(answer) == QStringLiteral("not_found") ||
                (error(answer).isEmpty() && ExternalStorageConsumer::data(answer) == expected),
            message);
}

void unregisteredCallers(Host *host) {
    MockModule stray;
    for (auto *caller : {static_cast<IModule *>(nullptr), static_cast<IModule *>(&stray)}) {
        ExternalStorageConsumer consumer(host, caller, QStringLiteral("unregistered"));
        require(consumer.storage() == nullptr, "an unregistered caller gets no storage");
    }
}

void samePlugin(Host *host, MockPlugin &plugin, const QString &run) {
    ExternalStorageConsumer first(host, plugin.module(), QStringLiteral("same-first"));
    ExternalStorageConsumer second(host, plugin.otherModule(), QStringLiteral("same-second"));
    require(first.storage() != second.storage(), "each module gets its own IStorage");
    require(first.storage() == host->storage(plugin.module()),
            "a module gets the same IStorage on every call");
    auto firstStorage = first.open();
    auto secondStorage = second.open();
    auto note = run + QStringLiteral("/shared.txt");
    requireDone(first.write(firstStorage, note, "shared", QStringLiteral("create")),
                "the first module writes a file");
    require(ExternalStorageConsumer::data(second.read(secondStorage, note)) == "shared",
            "modules of one plugin share its storage");
    requireError(second.read(firstStorage, note), "invalid_handle",
                 "a module cannot use the handle of another module of its plugin");
    requireError(first.read(secondStorage, note), "invalid_handle",
                 "handles of one plugin's modules are not interchangeable");
}

void crossPlugin(Host *host, IModule *mine, IModule *theirs, const QString &run) {
    ExternalStorageConsumer own(host, mine, QStringLiteral("cross-own"));
    ExternalStorageConsumer other(host, theirs, QStringLiteral("cross-other"));
    auto ownStorage = own.open();
    auto otherStorage = other.open();
    auto path = QStringLiteral("private-%1.txt").arg(run);
    requireDone(other.write(otherStorage, path, "theirs", QStringLiteral("create")),
                "the other plugin writes a private file");
    requireError(own.read(ownStorage, path), "not_found", "plugins have separate roots");
    requireError(own.read(otherStorage, path), "invalid_handle",
                 "a handle of another plugin cannot read");
    requireError(own.write(otherStorage, path, "mine", QStringLiteral("replace")), "invalid_handle",
                 "a handle of another plugin cannot write");
    requireError(own.call({{QStringLiteral("op"), QStringLiteral("read")},
                           {QStringLiteral("storage"), otherStorage},
                           {QStringLiteral("path"), path},
                           {QStringLiteral("plugin"), QStringLiteral("mock.sync.plugin")},
                           {QStringLiteral("module"), QStringLiteral("mock.sync.module")}}),
                 "invalid_request", "a request naming its sender is refused");
    require(ExternalStorageConsumer::data(other.read(otherStorage, path)) == "theirs",
            "refused foreign writes leave the file");
    requireDone(other.remove(otherStorage, path, false), "the other plugin removes its file");
}

void bounds(Host *host, IModule *module, const QString &run) {
    ExternalStorageConsumer consumer(host, module, QStringLiteral("bounds"));
    auto storage = consumer.open();
    QByteArray chunk(64 * 1024, 'q');
    QStringList ids;
    // One runs and the others queue: the handle starts the next one only once the GUI
    // thread got the answer before it, so all of these are outstanding together.
    for (int i = 0; i <= IStorage::MAX_OUTSTANDING; ++i) {
        ids.append(
            consumer.send(writeRequest(storage, run + QStringLiteral("/bound-%1").arg(i), chunk)));
    }
    requireError(consumer.wait(ids.last()), "busy",
                 "a request over the outstanding bound answers busy");
    for (int i = 0; i < IStorage::MAX_OUTSTANDING; ++i) {
        requireDone(consumer.wait(ids.at(i)), "requests within the outstanding bound complete");
    }
    requireError(
        consumer.inspect(storage, run + QStringLiteral("/bound-%1").arg(IStorage::MAX_OUTSTANDING)),
        "not_found", "the busy request wrote nothing");
    requireDone(consumer.write(storage, run + QStringLiteral("/bound-after"), chunk,
                               QStringLiteral("create")),
                "the bound counts outstanding requests only");
}

void cancellation(Host *host, IModule *module, ExternalStorageConsumer &witness,
                  const QString &run) {
    ExternalStorageConsumer consumer(host, module, QStringLiteral("cancel"));
    auto storage = consumer.open();
    QByteArray running(IStorage::MAX_FILE_SIZE, 'r');
    QByteArray queuedBytes(1024, 'c');
    auto runningPath = run + QStringLiteral("/cancel-running.bin");
    auto queuedPath = run + QStringLiteral("/cancel-queued.bin");
    auto keptPath = run + QStringLiteral("/kept.txt");
    auto first = consumer.send(writeRequest(storage, runningPath, running));
    auto queued = consumer.send(writeRequest(storage, queuedPath, queuedBytes));
    auto kept = consumer.send(writeRequest(storage, keptPath, "kept"));
    consumer.cancel(queued);
    witness.cancel(kept);
    consumer.cancel(QStringLiteral("never-sent"));
    consumer.cancel(first);
    requireError(consumer.wait(queued), "cancelled",
                 "a cancelled queued request answers cancelled");
    requireError(consumer.wait(first), "cancelled",
                 "a cancelled running request answers cancelled");
    // `kept` starts only once the cancelled running request has finished on this
    // handle, so its own late answer was already dropped (a second answer would fail).
    requireDone(consumer.wait(kept), "another caller cannot cancel this caller's request");
    requireError(consumer.read(storage, queuedPath), "not_found",
                 "a request cancelled before it started changes nothing");
    requireWholeOrAbsent(consumer.read(storage, runningPath), running,
                         "a running write cancelled is absent or whole");
    consumer.cancel(kept);
    require(ExternalStorageConsumer::data(consumer.read(storage, keptPath)) == "kept",
            "cancelling an answered request changes nothing");
}

void droppedCaller(HostImpl *host, IModule *module, ExternalStorageConsumer &witness,
                   const QString &witness_storage, const QString &run) {
    ExternalStorageConsumer consumer(host, module, QStringLiteral("dropped"));
    auto storage = consumer.open();
    auto before = run + QStringLiteral("/before-drop.txt");
    auto during = run + QStringLiteral("/during-drop.bin");
    auto queuedPath = run + QStringLiteral("/queued-drop.txt");
    QByteArray running(IStorage::MAX_FILE_SIZE, 'd');
    requireDone(consumer.write(storage, before, "kept", QStringLiteral("create")),
                "the caller writes before it is dropped");
    auto pending = consumer.send(writeRequest(storage, during, running));
    auto queued = consumer.send(writeRequest(storage, queuedPath, "queued"));
    host->dropCaller(module);
    requireError(consumer.call({{QStringLiteral("op"), QStringLiteral("discover")}}), "revoked",
                 "a dropped IStorage answers revoked");
    settle(witness, witness_storage);

    ExternalStorageConsumer again(host, module, QStringLiteral("dropped-again"));
    require(again.storage() != nullptr && again.storage() != consumer.storage(),
            "a registered module gets a new IStorage after it was dropped");
    requireError(again.read(storage, before), "invalid_handle",
                 "a handle of a dropped session is stale");
    auto fresh = again.open();
    require(fresh != storage, "a new session gets a new handle");
    require(ExternalStorageConsumer::data(again.read(fresh, before)) == "kept",
            "a new session finds the stored files");
    requireError(again.read(fresh, queuedPath), "not_found",
                 "a request queued when the caller was dropped never runs");
    requireWholeOrAbsent(again.read(fresh, during), running,
                         "a write running when the caller was dropped is absent or whole");
    require(!consumer.answered(pending) && !consumer.answered(queued),
            "a dropped caller's outstanding requests are never answered");
}

void lockedStorage(Host *host, IModule *module, const QString &password, const QString &run) {
    ExternalStorageConsumer consumer(host, module, QStringLiteral("locked"));
    auto storage = consumer.open();
    auto path = run + QStringLiteral("/locked.txt");
    requireDone(consumer.write(storage, path, "before", QStringLiteral("create")),
                "the caller writes before the lock");
    auto late = consumer.send({{QStringLiteral("op"), QStringLiteral("read")},
                               {QStringLiteral("storage"), storage},
                               {QStringLiteral("path"), path}});
    finishWorker(consumer);
    require(!consumer.answered(late), "the computed read is still queued for GUI delivery");
    ::state_lock();
    auto withheld = consumer.wait(late);
    requireError(withheld, "locked", "locking fences a read already computed by its worker");
    require(!withheld.contains(QStringLiteral("data")), "a late locked response carries no bytes");
    requireError(consumer.read(storage, path), "locked", "locked storage refuses a read");
    requireError(consumer.write(storage, path, "after", QStringLiteral("replace")), "locked",
                 "locked storage refuses a write");
    requireError(consumer.call({{QStringLiteral("op"), QStringLiteral("discover")}}), "locked",
                 "locked storage refuses discovery");
    require(::state_unlock_with_password(rust::String(password.toStdString())).empty(),
            "the app state unlocks again");
    require(ExternalStorageConsumer::data(consumer.read(storage, path)) == "before",
            "the session works again once unlocked, and the refused write changed nothing");
}

// The former single-blob API is gone from the bridge, so no legacy blob can be seeded
// here: the Rust storage tests cover migrating one.
void migration(Host *host, IModule *module, const QString &run) {
    ExternalStorageConsumer consumer(host, module, QStringLiteral("migrate"));
    auto storage = consumer.open();
    auto destination = QStringLiteral("legacy-%1.json").arg(run);
    auto none = consumer.migrate(storage, destination);
    requireDone(none, "migration answers when there is nothing to migrate");
    require(none.value(QStringLiteral("migrated")).isBool() &&
                !none.value(QStringLiteral("migrated")).toBool(),
            "a plugin without a legacy blob migrates nothing");
    requireError(consumer.read(storage, destination), "not_found",
                 "migrating nothing creates no file");
    requireError(consumer.migrate(storage, QStringLiteral("../escape")), "invalid_path",
                 "a migration destination is a logical path");
}

#ifdef SILENT_STORAGE_FIXTURE
const QString FIXTURE_ID = QStringLiteral("storage.fixture");
// The fixture library installed under another id: its meta() still claims FIXTURE_ID.
const QString CLAIMER_ID = QStringLiteral("storage.claimer");

// The error a caller gets on discovery, empty when it reached its storage.
auto discovery(ExternalStorageConsumer &consumer) -> QString {
    require(consumer.storage() != nullptr, "an active module gets a storage");
    return error(consumer.call({{QStringLiteral("op"), QStringLiteral("discover")}}));
}

auto sha256(const QByteArray &bytes) -> QString {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void installFixture(const QString &plugin_id, const QByteArray &library) {
    auto installed = installBundle(plugin_id, QStringLiteral("Storage fixture"), library);
    if (!installed.is_ok) {
        qFatal("storage smoke: fixture install failed: %s", installed.error.c_str());
    }
}

auto fixtureModule(PluginRegistry *registry, const QString &module_id) -> IModule * {
    for (auto *plugin : registry->plugins()) {
        for (auto *module : plugin->modules()) {
            if (module->meta().value(QStringLiteral("id")) == module_id) {
                return module;
            }
        }
    }
    qFatal("storage smoke: fixture module %s is not loaded", qPrintable(module_id));
}

auto stateGrant(const QString &hash) -> PluginGrant {
    return ::app_plugin_answers(rust::String(FIXTURE_ID.toStdString()),
                                rust::String(hash.toStdString()))
        .state;
}

void setStateGrant(const QString &hash, PluginGrant grant) {
    auto answers = ::app_plugin_answers(rust::String(FIXTURE_ID.toStdString()),
                                        rust::String(hash.toStdString()));
    answers.plugin_id = rust::String(FIXTURE_ID.toStdString());
    answers.hash = rust::String(hash.toStdString());
    answers.state = grant;
    ::app_set_plugin_access(answers);
}

// Names and contents below the visible plugin root are sealed.
void requireSealedRoot(const QString &run, const QByteArray &marker) {
    QDir root(QDir::home().filePath(QStringLiteral(".silent/%1").arg(FIXTURE_ID)));
    require(root.exists(), "the plugin root is visible below the data directory");
    QDirIterator it(root.path(),
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    int files = 0;
    while (it.hasNext()) {
        QFileInfo info(it.next());
        require(!info.isSymLink(), "the plugin root holds no link");
        require(!info.fileName().contains(run) && !info.fileName().contains(QStringLiteral("keep")),
                "logical names are concealed on disk");
        if (info.isFile()) {
            ++files;
            QFile file(info.filePath());
            require(file.open(QIODevice::ReadOnly), "a sealed file opens");
            require(!file.readAll().contains(marker), "file contents are encrypted on disk");
        }
    }
    require(files > 0, "the plugin root holds the sealed tree");
}

struct ExternalRun {
    QString hash;
    // A handle of the first library version's session.
    QString storage;
    QString keep;
    QString during;
    QString queued;
    QByteArray running;
    QByteArray marker;
};

void externalGrants(HostImpl *host, PluginRegistry *registry, ExternalStorageConsumer &witness,
                    const QString &witness_storage, const QString &run, ExternalRun &state) {
    auto *first = fixtureModule(registry, QStringLiteral("storage.first"));
    auto *second = fixtureModule(registry, QStringLiteral("storage.second"));
    require(HostImpl::caller(first).external && HostImpl::caller(first).pluginHash == state.hash,
            "fixture modules are external callers with their library hash");

    // Answered before the first acquisition, so the host asks nothing.
    setStateGrant(state.hash, PluginGrant::Denied);
    ExternalStorageConsumer one(host, first, QStringLiteral("external-first"));
    require(discovery(one) == QStringLiteral("denied"),
            "a denied Saved Data grant refuses the storage");
    setStateGrant(state.hash, PluginGrant::Allowed);
    ExternalStorageConsumer two(host, second, QStringLiteral("external-second"));
    state.storage = one.open();
    auto twoStorage = two.open();
    requireDone(one.mkdir(state.storage, run), "an allowed external plugin creates a directory");
    requireDone(one.write(state.storage, state.keep, state.marker, QStringLiteral("create")),
                "an allowed external plugin writes a file");
    require(ExternalStorageConsumer::data(two.read(twoStorage, state.keep)) == state.marker,
            "external modules of one plugin share its storage");
    requireError(two.read(state.storage, state.keep), "invalid_handle",
                 "external modules of one plugin do not share handles");
    requireError(one.read(witness_storage, QStringLiteral("barrier.txt")), "invalid_handle",
                 "an external plugin cannot use a built-in plugin's handle");
    requireError(witness.read(state.storage, state.keep), "invalid_handle",
                 "a built-in plugin cannot use an external plugin's handle");

    setStateGrant(state.hash, PluginGrant::Denied);
    requireError(one.read(state.storage, state.keep), "denied",
                 "a revoked grant refuses the next request");
    setStateGrant(state.hash, PluginGrant::Allowed);
    // The read queues behind the write, and the handle starts it only after the GUI
    // thread got the write's answer: the grant is revoked before it can run.
    auto blocking =
        one.send(writeRequest(state.storage, run + QStringLiteral("/blocking.bin"), state.running));
    auto queued = one.send({{QStringLiteral("op"), QStringLiteral("read")},
                            {QStringLiteral("storage"), state.storage},
                            {QStringLiteral("path"), state.keep}});
    setStateGrant(state.hash, PluginGrant::Denied);
    auto withheld = one.wait(queued);
    require(error(withheld) == QStringLiteral("denied") &&
                !withheld.contains(QStringLiteral("data")),
            "a grant revoked while a request is queued withholds its data");
    auto blocked = error(one.wait(blocking));
    require(blocked.isEmpty() || blocked == QStringLiteral("denied"),
            "a write running when the grant is revoked completes or is denied");
    setStateGrant(state.hash, PluginGrant::Allowed);
    require(ExternalStorageConsumer::data(one.read(state.storage, state.keep)) == state.marker,
            "restoring the grant restores access");

    auto late = one.send({{QStringLiteral("op"), QStringLiteral("read")},
                          {QStringLiteral("storage"), state.storage},
                          {QStringLiteral("path"), state.keep}});
    finishWorker(one);
    require(!one.answered(late), "the completed read has not reached the GUI consumer");
    setStateGrant(state.hash, PluginGrant::Denied);
    auto lateAnswer = one.wait(late);
    requireError(lateAnswer, "denied", "grant revocation fences a computed queued response");
    require(!lateAnswer.contains(QStringLiteral("data")),
            "revocation removes computed secret bytes");
    setStateGrant(state.hash, PluginGrant::Allowed);

    auto lateUnload = two.send({{QStringLiteral("op"), QStringLiteral("read")},
                                {QStringLiteral("storage"), twoStorage},
                                {QStringLiteral("path"), state.keep}});
    finishWorker(two);
    require(!two.answered(lateUnload), "the second module has a computed queued response");

    auto pending = one.send(writeRequest(state.storage, state.during, state.running));
    auto later = one.send(writeRequest(state.storage, state.queued, "queued"));
    require(registry->setEnabled(FIXTURE_ID, false),
            "the fixture plugin unloads with requests outstanding");
    requireError(one.call({{QStringLiteral("op"), QStringLiteral("discover")}}), "revoked",
                 "the IStorage of an unloaded plugin answers revoked");
    settle(witness, witness_storage);
    require(!one.answered(pending) && !one.answered(later),
            "an unloaded plugin's outstanding requests are never answered");
    require(!two.answered(lateUnload), "unload also suppresses an already computed response");
}

void externalUpgrade(HostImpl *host, PluginRegistry *registry, const QByteArray &library,
                     const QString &run, const ExternalRun &state) {
    // Bytes after the ELF image change the hash and nothing the loader reads.
    auto upgraded = library + QByteArray("\nstorage fixture upgrade ") + run.toLatin1();
    auto hash = sha256(upgraded);
    require(hash != state.hash, "the upgraded library has its own hash");
    installFixture(FIXTURE_ID, upgraded);
    require(stateGrant(hash) == PluginGrant::Unasked,
            "a new library version inherits no Saved Data grant");
    registry->rescanDiscoveredPlugins();
    require(registry->setEnabled(FIXTURE_ID, true) && registry->pluginHash(FIXTURE_ID) == hash,
            "the upgraded fixture loads under its new hash");
    auto *module = fixtureModule(registry, QStringLiteral("storage.first"));

    // The host asks on the first acquisition: the prompt is denied.
    StoragePrompts prompts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &prompts, &StoragePrompts::onTick,
                     Qt::UniqueConnection);
    timer.start(10);
    ExternalStorageConsumer after(host, module, QStringLiteral("external-upgraded"));
    timer.stop();
    require(stateGrant(hash) == PluginGrant::Denied, "the denied prompt is saved for the new hash");
    require(discovery(after) == QStringLiteral("denied"),
            "the upgraded library is refused until its own grant is allowed");

    setStateGrant(hash, PluginGrant::Allowed);
    requireError(after.read(state.storage, state.keep), "invalid_handle",
                 "a handle of the old library's session is stale");
    auto storage = after.open();
    require(ExternalStorageConsumer::data(after.read(storage, state.keep)) == state.marker,
            "the upgraded plugin finds its storage at the same root");
    requireError(after.read(storage, state.queued), "not_found",
                 "a request queued at unload never runs");
    requireWholeOrAbsent(after.read(storage, state.during), state.running,
                         "a write running at unload is absent or whole");
    require(stateGrant(state.hash) == PluginGrant::Allowed, "the old library keeps its own grant");
    requireSealedRoot(run, state.marker);
    requireDone(after.remove(storage, run, true), "the run's tree is removed");
}

// An installed library whose meta() claims another installed plugin's id is refused at
// load: it never runs as that plugin, with its hash, grants or storage (ws-plaude/silent#60).
void claimedIdentity(PluginRegistry *registry, const QByteArray &library, const QString &run) {
    // Its own bytes, so its own hash, while FIXTURE_ID is loaded and allowed Saved Data.
    auto claimer = library + QByteArray("\nstorage fixture claimer ") + run.toLatin1();
    installFixture(CLAIMER_ID, claimer);
    registry->rescanDiscoveredPlugins();
    require(!registry->setEnabled(CLAIMER_ID, true),
            "a library claiming another installed plugin's id does not load");
    require(!registry->isPluginEnabled(CLAIMER_ID) &&
                registry->pluginLoadError(CLAIMER_ID).contains(FIXTURE_ID),
            "the refused library reports the id it claimed");
    int claimed = 0;
    for (auto *plugin : registry->plugins()) {
        for (auto *module : plugin->modules()) {
            auto caller = HostImpl::caller(module);
            require(caller.pluginHash != sha256(claimer),
                    "no active module runs with the refused library's hash");
            if (module->meta().value(QStringLiteral("id")) == QStringLiteral("storage.first")) {
                ++claimed;
                require(caller.pluginId == FIXTURE_ID &&
                            caller.pluginHash == registry->pluginHash(FIXTURE_ID),
                        "only the installed plugin runs as itself");
            }
        }
    }
    require(claimed == 1, "the refused library left no module behind");
}

// A module's identity is fixed when it is activated: its plugin and itself reporting
// other ids afterwards keep the caller, hash, grant and storage they were activated with.
void changedIdentity(HostImpl *host, PluginRegistry *registry, const QString &run) {
    auto *module = fixtureModule(registry, QStringLiteral("storage.first"));
    auto hash = registry->pluginHash(FIXTURE_ID);
    auto path = QStringLiteral("identity-%1.txt").arg(run);
    ExternalStorageConsumer before(host, module, QStringLiteral("identity-before"));
    auto storage = before.open();
    requireDone(before.write(storage, path, "activated", QStringLiteral("create")),
                "the activated plugin writes a file");

    module->parent()->setProperty("claim", CLAIMER_ID);
    module->setProperty("claim", QStringLiteral("storage.second"));
    require(module->meta().value(QStringLiteral("id")) == QStringLiteral("storage.second"),
            "the module reports another id after activation");
    auto caller = HostImpl::caller(module);
    require(caller.pluginId == FIXTURE_ID && caller.pluginHash == hash && caller.external &&
                caller.moduleId == QStringLiteral("storage.first"),
            "the caller keeps the identity it was activated with");
    require(ExternalStorageConsumer::data(before.read(storage, path)) == "activated",
            "the open session keeps its storage");

    // A new acquisition reads the caller again, after the change.
    host->dropCaller(module);
    ExternalStorageConsumer after(host, module, QStringLiteral("identity-after"));
    require(after.storage() != nullptr, "the changed module still gets a storage");
    auto fresh = after.open();
    require(ExternalStorageConsumer::data(after.read(fresh, path)) == "activated",
            "a new session opens the activated plugin's root, not the claimed one");
    setStateGrant(hash, PluginGrant::Denied);
    requireError(after.read(fresh, path), "denied",
                 "the activated plugin's grant still governs the changed module");
    setStateGrant(hash, PluginGrant::Allowed);
    requireDone(after.remove(fresh, path, false), "the identity file is removed");

    module->parent()->setProperty("claim", QVariant());
    module->setProperty("claim", QVariant());
}

void externalPlugin(HostImpl *host, PluginRegistry *registry, ExternalStorageConsumer &witness,
                    const QString &witness_storage, const QString &run) {
    QFile file(QString::fromUtf8(SILENT_STORAGE_FIXTURE));
    require(file.open(QIODevice::ReadOnly), "the fixture library opens");
    auto library = file.readAll();
    installFixture(FIXTURE_ID, library);
    registry->rescanDiscoveredPlugins();
    require(registry->setEnabled(FIXTURE_ID, true), "the fixture plugin loads");
    ExternalRun state{.hash = registry->pluginHash(FIXTURE_ID),
                      .keep = run + QStringLiteral("/keep.txt"),
                      .during = run + QStringLiteral("/during-unload.bin"),
                      .queued = run + QStringLiteral("/queued-unload.txt"),
                      .running = QByteArray(IStorage::MAX_FILE_SIZE, 'u'),
                      .marker = QByteArray("storage-fixture-plaintext-") + run.toLatin1()};
    require(state.hash == sha256(library), "the fixture runs under its library hash");
    externalGrants(host, registry, witness, witness_storage, run, state);
    externalUpgrade(host, registry, library, run, state);
    claimedIdentity(registry, library, run);
    changedIdentity(host, registry, run);
    require(registry->setEnabled(FIXTURE_ID, false), "the upgraded fixture unloads");
    require(std::string(::plugin_remove(rust::String(FIXTURE_ID.toStdString()))).empty(),
            "the fixture plugin is removed");
    require(std::string(::plugin_remove(rust::String(CLAIMER_ID.toStdString()))).empty(),
            "the claiming plugin is removed");
}
#endif
} // namespace

auto storageSmoke(const QString &password) -> bool {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the host supports caller teardown");
    auto ownedPlugin = std::make_unique<MockPlugin>();
    auto &plugin = *ownedPlugin;
    AppController::get()->registerBuiltinPlugin(std::move(ownedPlugin));
    auto *registry = AppController::get()->pluginRegistry();
    require(registry != nullptr, "the plugin registry exists");
    MockSyncPlugin other;
    registry->registerBuiltin(&other);
    // Paths are unique per run, so a reused HOME never collides with an earlier run.
    auto run = QUuid::createUuid().toString(QUuid::WithoutBraces);

    ExternalStorageConsumer witness(host, other.module(), QStringLiteral("witness"));
    auto witnessStorage = witness.open();
    settle(witness, witnessStorage);
    ExternalStorageConsumer contract(host, plugin.module(), QStringLiteral("contract"));
    contract.runContract(run);

    unregisteredCallers(host);
    samePlugin(host, plugin, run);
    crossPlugin(host, plugin.module(), other.module(), run);
    bounds(host, plugin.module(), run);
    cancellation(host, plugin.module(), witness, run);
    droppedCaller(host, plugin.otherModule(), witness, witnessStorage, run);
    lockedStorage(host, plugin.module(), password, run);
    migration(host, plugin.module(), run);
#ifdef SILENT_STORAGE_FIXTURE
    externalPlugin(host, registry, witness, witnessStorage, run);
#endif
    // A reused HOME keeps the plugin trees: each run removes its own, within their bounds.
    auto storage = contract.open();
    requireDone(contract.remove(storage, run, true), "the run's tree is removed");
    requireError(contract.inspect(storage, run), "not_found", "the run left nothing behind");
    return true;
}

} // namespace smoke
