#include "quit_shutdown_smoke.h"

#include "AppController.h"
#include "ExternalStorageConsumer.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include <common.h>
#include <memory>
#include <utility>

namespace smoke {
namespace {

// The stuck module holds the quit for the whole bound; the writes land well within it.
constexpr int QUIT_BOUND_MS = 1500;
constexpr qint64 QUIT_SLACK_MS = 3000;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("quit shutdown smoke: %s", message);
    }
}

void requireWritten(const QHash<QString, QJsonObject> &answers, const QString &id,
                    const char *message) {
    require(answers.contains(id) && ExternalStorageConsumer::error(answers.value(id)).isEmpty(),
            message);
}

} // namespace

QuitModule::QuitModule(QString id, bool stuck, QObject *parent)
    : IModule(parent),
      m_id(std::move(id)),
      m_stuck(stuck) {
}

auto QuitModule::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), m_id}, {QStringLiteral("name"), m_id}};
}

auto QuitModule::list() -> ReqId {
    return ++m_next;
}

auto QuitModule::autoStart() -> ReqId {
    return ++m_next;
}

void QuitModule::startInstance([[maybe_unused]] const QString &id) {
}

void QuitModule::shutdown(ReqId req_id) {
    ++m_shutdown_calls;
    if (m_stuck) {
        return;
    }
    m_shutdown_req = req_id;
    write(QStringLiteral("shutdown"), QStringLiteral("shutdown.txt"));
}

void QuitModule::prepare(IStorage *storage, const QString &handle, const QString &path) {
    m_storage = storage;
    m_handle = handle;
    m_path = path;
    connect(storage, &IStorage::completed, this, &QuitModule::onCompleted, qontrol::UNIQUE);
}

auto QuitModule::shutdownCalls() const -> int {
    return m_shutdown_calls;
}

auto QuitModule::answers() const -> const QHash<QString, QJsonObject> & {
    return m_answers;
}

void QuitModule::onCompleted(const QByteArray &message) {
    auto answer = QJsonDocument::fromJson(message).object();
    auto id = answer.value(QStringLiteral("id")).toString();
    if (id != QStringLiteral("shutdown")) {
        return;
    }
    m_answers.insert(id, answer);
    emit shutdownFinished(m_shutdown_req, ExternalStorageConsumer::error(answer));
}

void QuitModule::write(const QString &id, const QString &name) {
    if (m_storage == nullptr) {
        return;
    }
    QJsonObject request{{QStringLiteral("id"), id},
                        {QStringLiteral("op"), QStringLiteral("write")},
                        {QStringLiteral("storage"), m_handle},
                        {QStringLiteral("path"), m_path + QLatin1Char('/') + name},
                        {QStringLiteral("data"), QString::fromLatin1(id.toUtf8().toBase64())},
                        {QStringLiteral("mode"), QStringLiteral("upsert")}};
    m_storage->request(QJsonDocument(request).toJson(QJsonDocument::Compact));
}

QuitPlugin::QuitPlugin()
    : m_saver(new QuitModule(QStringLiteral("quit.saver"), false, this)),
      m_stuck(new QuitModule(QStringLiteral("quit.stuck"), true, this)) {
}

auto QuitPlugin::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), QStringLiteral("quit.plugin")},
            {QStringLiteral("name"), QStringLiteral("Quit Plugin")}};
}

auto QuitPlugin::modules() const -> QList<IModule *> {
    return {m_saver, m_stuck};
}

auto QuitPlugin::saver() const -> QuitModule * {
    return m_saver;
}

auto QuitPlugin::stuck() const -> QuitModule * {
    return m_stuck;
}

void quitShutdownSmoke() {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the real host singleton runs the quit");
    auto owned = std::make_unique<QuitPlugin>();
    auto *plugin = owned.get();
    AppController::get()->registerBuiltinPlugin(std::move(owned));
    auto *registry = AppController::get()->pluginRegistry();
    require(registry != nullptr, "the plugin registry exists");
    auto run = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ExternalStorageConsumer files(host, plugin->saver(), QStringLiteral("quit"));
    auto storage = files.open();
    require(ExternalStorageConsumer::error(files.mkdir(storage, run)).isEmpty(),
            "the run directory is made");
    plugin->saver()->prepare(files.storage(), storage, run);

    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    require(QCoreApplication::exec() == 0, "the application quit path returns");
    require(plugin->saver()->shutdownCalls() == 0 && plugin->stuck()->shutdownCalls() == 0,
            "quitting the event loop alone shuts no module down");

    QElapsedTimer elapsed;
    elapsed.start();
    registry->shutdownAtQuit(QUIT_BOUND_MS);
    auto waited = elapsed.elapsed();
    require(plugin->saver()->shutdownCalls() == 1 && plugin->stuck()->shutdownCalls() == 1,
            "the quit shuts every enabled module down once");
    requireWritten(plugin->saver()->answers(), QStringLiteral("shutdown"),
                   "the write made during shutdown is answered");
    require(waited >= QUIT_BOUND_MS, "the quit waits for a module still shutting down");
    require(waited < QUIT_BOUND_MS + QUIT_SLACK_MS,
            "a module that never answers holds the quit up to the bound only");
    require(ExternalStorageConsumer::data(files.read(
                storage, run + QStringLiteral("/shutdown.txt"))) == QByteArray("shutdown"),
            "the shutdown file is on disk");
    require(ExternalStorageConsumer::error(files.remove(storage, run, true)).isEmpty(),
            "the run's tree is removed");
}

} // namespace smoke
