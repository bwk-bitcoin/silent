#include "PluginAccess.h"

#include <QThread>
#include <common.h>
#include <string>

namespace pluginaccess {

Job::~Job() {
    release();
}

auto Job::takeSession() -> uint64_t {
    auto session = m_session.exchange(0);
    if (session != 0 && m_released.load()) {
        ::storage_session_revoke(session);
        return 0;
    }
    return session;
}

void Job::release() {
    m_released.store(true);
    takeSession();
}

auto request(const QString &plugin_id, const QString &hash) -> PluginAccessRequest {
    return ::app_plugin_access_request(rust::String(plugin_id.toStdString()),
                                       rust::String(hash.toStdString()));
}

auto history(const QString &plugin_id) -> PluginAccessHistory {
    return ::app_plugin_access_history(rust::String(plugin_id.toStdString()));
}

auto save(const PluginAccess &access, uint64_t expected_epoch) -> QString {
    return QString::fromStdString(std::string(::app_set_plugin_access(access, expected_epoch)));
}

// The thread captures the job, which it outlives, and copies of the values only.
void Job::request(uint64_t revision, const QString &plugin_id, const QString &hash) {
    start(QThread::create([this, revision, plugin_id, hash] {
        emit requestRead(revision, pluginaccess::request(plugin_id, hash));
    }));
}

void Job::history(uint64_t revision, const QString &plugin_id) {
    start(QThread::create([this, revision, plugin_id] {
        emit historyRead(revision, pluginaccess::history(plugin_id));
    }));
}

void Job::save(uint64_t revision, const PluginAccess &access, uint64_t expected_epoch) {
    start(QThread::create([this, revision, access, expected_epoch] {
        emit saved(revision, pluginaccess::save(access, expected_epoch));
    }));
}

void Job::saveIfCurrent(uint64_t revision, const PluginAccess &access, uint64_t expected_epoch,
                        uint64_t expected_version) {
    start(QThread::create([this, revision, access, expected_epoch, expected_version] {
        auto error = QString::fromStdString(std::string(
            ::app_set_plugin_access_if_current(access, expected_epoch, expected_version)));
        emit saved(revision, error);
    }));
}

void Job::openStorage(uint64_t revision, const QString &plugin_id, const QString &hash,
                      bool external) {
    start(QThread::create([this, revision, plugin_id, hash, external] {
        m_session.store(::storage_session_open(rust::String(plugin_id.toStdString()),
                                               rust::String(hash.toStdString()), external));
        if (m_released.load()) {
            release();
        }
        emit storageOpened(revision);
    }));
}

// Neither has a parent: closing the view that started the job never destroys a running
// thread. The answer is queued before finished, so it is delivered before the job goes.
void Job::start(QThread *thread) {
    connect(thread, &QThread::finished, this, &QObject::deleteLater, qontrol::UNIQUE);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
    thread->start();
}

} // namespace pluginaccess
