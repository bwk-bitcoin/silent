#pragma once

#include <QFile>
#include <QObject>
#include <QTimer>
#include <cstdint>

class HostImpl;
class IModule;
class PluginRegistry;

namespace smoke {

// Holds the real interprocess app-state lock, as another Silent process would.
class AppStateLock final {
public:
    void hold();
    void release();

private:
    QFile m_file;
};

// Drives the GUI from its own timer while Host::requestAccess() waits in its nested event
// loop: each tick proves the GUI thread still runs, and acts once the step is due.
class AccessDriver final : public QObject {
    Q_OBJECT

public:
    enum class Step {
        // Withdraws the request waiting on a held read.
        HeldRead,
        // Withdraws the open prompt before it is answered.
        Prompt,
        // Allows with the save held, then releases it.
        HeldSave,
        Publication,
    };

    AccessDriver(HostImpl *host, IModule *module, AppStateLock *lock);
    void start(Step step);
    void stop();
    void holdPublication(PluginRegistry *registry);

    [[nodiscard]] auto ticks() const -> int {
        return m_ticks;
    }

    // A real access prompt was shown.
    [[nodiscard]] auto prompted() const -> bool {
        return m_prompted;
    }

    // The prompt stayed open, unsaved and without error, while its save was held.
    [[nodiscard]] auto heldDuringSave() const -> bool {
        return m_held_during_save;
    }

    [[nodiscard]] auto removed() const -> bool {
        return m_removed;
    }

    [[nodiscard]] auto stopError() const -> QString {
        return m_stop_error;
    }

    [[nodiscard]] auto abandonedSessionChecked() const -> bool {
        return m_abandoned_session_checked;
    }

signals:
    void releaseWorker();

public slots:
    void onTick();
    void onStopped(const QString &plugin_id, bool removed, const QString &error);
    void onAbandonedStorageOpened(uint64_t revision);

protected:
    void heldSave();

private:
    HostImpl *m_host = nullptr;
    IModule *m_module = nullptr;
    AppStateLock *m_lock = nullptr;
    QTimer m_timer;
    PluginRegistry *m_registry = nullptr;
    Step m_step = Step::HeldRead;
    int m_ticks = 0;
    bool m_prompted = false;
    bool m_held_during_save = false;
    int m_saved_at = 0;
    bool m_removed = false;
    QString m_stop_error;
    bool m_abandoned_session_checked = false;
};

void accessAsyncSmoke();
void publicationHeartbeatSmoke();

} // namespace smoke
