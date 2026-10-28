#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <silent.h>

class QThread;

// The user's answers to external plugin libraries, kept in the app state by library
// hash.
namespace pluginaccess {

// The answers stored for the library `hash` and the plugin's storage epoch, read when a
// grant request starts. Refused while the plugin's removal is pending. Takes the
// app-state locks: off the GUI thread, through Job.
auto request(const QString &plugin_id, const QString &hash) -> PluginAccessRequest;
// Every answer stored for the plugin `plugin_id`, one entry per library hash, and its
// storage epoch, read when Settings shows them. Refused like `request()`.
auto history(const QString &plugin_id) -> PluginAccessHistory;
// Saves `access` against the epoch its request or history was read with: the error,
// empty once saved.
auto save(const PluginAccess &access, uint64_t expected_epoch) -> QString;

// Runs one read, save or storage session open on its own thread, from values copied
// when it starts, and answers on the GUI thread with the `revision` it was given. It
// deletes itself once its thread finished, so whoever started it may go meanwhile: the
// answer then reaches nobody. Connect the answer before starting it.
class Job final : public QObject {
    Q_OBJECT

public:
    ~Job() override;
    // A released job revokes its session instead of transferring it.
    auto takeSession() -> uint64_t;
    void release();
    void request(uint64_t revision, const QString &plugin_id, const QString &hash);
    void history(uint64_t revision, const QString &plugin_id);
    void save(uint64_t revision, const PluginAccess &access, uint64_t expected_epoch);
    // Prompt saves also refuse answers superseded since the request was read.
    void saveIfCurrent(uint64_t revision, const PluginAccess &access, uint64_t expected_epoch,
                       uint64_t expected_version);
    // Opens the storage session of the plugin the host authenticated: 0 when refused.
    void openStorage(uint64_t revision, const QString &plugin_id, const QString &hash,
                     bool external);

signals:
    void requestRead(uint64_t revision, PluginAccessRequest request);
    void historyRead(uint64_t revision, PluginAccessHistory history);
    void saved(uint64_t revision, QString error);
    void storageOpened(uint64_t revision);

protected:
    void start(QThread *thread);

private:
    std::atomic<uint64_t> m_session{0};
    std::atomic<bool> m_released{false};
};

} // namespace pluginaccess
