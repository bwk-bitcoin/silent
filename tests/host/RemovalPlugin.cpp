#include "interfaces/host.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"
#include "interfaces/storage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QWidget>
#include <common.h>
#include <cstdio>
#include <cstdlib>

namespace {
void record(const char *event) {
    auto *path = std::getenv("SILENT_REMOVAL_SENTINEL");
    if (path == nullptr) {
        return;
    }
    auto *file = std::fopen(path, "ab");
    if (file == nullptr || std::fputs(event, file) < 0 || std::fclose(file) != 0) {
        qFatal("removal fixture sentinel failed");
    }
}

struct Finalizer {
    ~Finalizer() {
        record("unload\n");
    }
} finalizer;
} // namespace

class Q_DECL_HIDDEN RemovalPanel final : public QWidget {
    Q_OBJECT
public:
    ~RemovalPanel() override {
        record("widget\n");
    }
};

class Q_DECL_HIDDEN RemovalModule final : public IModule, public IInstance {
    Q_OBJECT
    Q_INTERFACES(IInstance)

public:
    explicit RemovalModule(QObject *parent) : IModule(parent) {
    }

    ~RemovalModule() override {
        if (m_worker != nullptr || m_open_pending || m_write_pending || m_panel != nullptr) {
            qFatal("native worker destroyed before completion");
        }
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("removal.module")}};
    }

    auto list() -> ReqId override {
        emit instances(1, {{QStringLiteral("removal.account"), QStringLiteral("Native worker")}});
        return 1;
    }

    void deleteAccount(ReqId req_id, const QString &id) override {
        setProperty("deleteCalls", property("deleteCalls").toInt() + 1);
        emit accountDeleted(
            req_id, id, QStringLiteral("fixture deletion must not be dispatched while stopping"));
    }

    auto autoStart() -> ReqId override {
        m_storage = Host::get()->storage(this);
        if (m_storage == nullptr) {
            qFatal("native fixture requires its authenticated storage handle");
        }
        connect(m_storage, &IStorage::completed, this, &RemovalModule::onStorageCompleted,
                qontrol::UNIQUE);
        m_open_pending = true;
        m_storage->request(R"({"id":"open","op":"open","storage":"private"})");
        m_worker = QThread::create([this] {
            m_release.acquire();
            record("worker\n");
        });
        connect(m_worker, &QThread::finished, this, &RemovalModule::onWorkerFinished,
                qontrol::UNIQUE);
        m_worker->start();
        Host::get()->registerInstance(id(), this, this);
        m_panel = new RemovalPanel;
        m_section =
            Host::get()->addSettingSection(m_panel, this, QStringLiteral("Native closed UI"));
        emit IModule::autoStart(2, {});
        return 2;
    }

    [[nodiscard]] auto id() const -> QString override {
        return QStringLiteral("removal.instance");
    }

    auto account() -> IAccount * override {
        return nullptr;
    }

    auto feed() -> IFeed * override {
        return nullptr;
    }

    auto signer() -> ISigner * override {
        return nullptr;
    }

    auto theme() -> IThemeProvider * override {
        return nullptr;
    }

    void stop() override {
        if (m_section != 0) {
            Host::get()->delSettingSection(m_section);
            m_section = 0;
        }
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }

    void shutdown(ReqId req_id) override {
        if (std::getenv("SILENT_REMOVAL_REFUSE") != nullptr) {
            emit shutdownFinished(req_id, QStringLiteral("deliberate native refusal"));
            return;
        }
        m_request = req_id;
        finishShutdown();
    }

public slots:

    void releaseWorker() {
        m_release.release();
    }

    void onWorkerFinished() {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
        finishShutdown();
    }

    void onStorageCompleted(const QByteArray &message) {
        auto response = QJsonDocument::fromJson(message).object();
        auto id = response.value(QStringLiteral("id")).toString();
        auto error = response.value(QStringLiteral("error")).toString();
        if (id == QStringLiteral("open")) {
            m_open_pending = false;
            if (!error.isEmpty()) {
                qFatal("native fixture storage open failed: %s", qPrintable(error));
            }
            m_storage_id = response.value(QStringLiteral("storage")).toString();
            finishShutdown();
        } else if (id == QStringLiteral("flush")) {
            m_write_pending = false;
            m_flushed = error.isEmpty();
            record(m_flushed ? "flush\n" : "flush-failed\n");
            auto request = m_request;
            m_request = 0;
            emit shutdownFinished(request, error);
        }
    }

protected:
    void finishShutdown() {
        if (m_request == 0 || m_worker != nullptr || m_open_pending || m_write_pending) {
            return;
        }
        if (m_flushed) {
            auto request = m_request;
            m_request = 0;
            emit shutdownFinished(request, QString());
            return;
        }
        m_write_pending = true;
        QJsonObject request{{QStringLiteral("id"), QStringLiteral("flush")},
                            {QStringLiteral("op"), QStringLiteral("write")},
                            {QStringLiteral("storage"), m_storage_id},
                            {QStringLiteral("path"), QStringLiteral("shutdown-flush")},
                            {QStringLiteral("data"), QStringLiteral("bmF0aXZlLWZpbmFsLXdyaXRl")},
                            {QStringLiteral("mode"), QStringLiteral("upsert")}};
        m_storage->request(QJsonDocument(request).toJson(QJsonDocument::Compact));
    }

private:
    QThread *m_worker = nullptr;
    ReqId m_request = 0;
    QSemaphore m_release;
    QPointer<IStorage> m_storage;
    QString m_storage_id;
    bool m_open_pending = false;
    bool m_write_pending = false;
    bool m_flushed = false;
    QPointer<RemovalPanel> m_panel;
    Host::SectionId m_section = 0;
};

class Q_DECL_HIDDEN RemovalPlugin final : public QObject, public IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SILENT_IPlugin_IID)
    Q_INTERFACES(IPlugin)

public:
    RemovalPlugin() : m_module(new RemovalModule(this)) {
        record("constructor\n");
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("removal.fixture")}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {m_module};
    }

private:
    RemovalModule *m_module;
};

#include "RemovalPlugin.moc"
