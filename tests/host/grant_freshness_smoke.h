#pragma once

#include "host/PluginAccess.h"
#include "interfaces/types.h"

#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QTimer>

class HostImpl;
class IModule;
class QEvent;

namespace modal {
class PluginAccessPrompt;
}

namespace smoke {
class AppStateLock;

class GrantFreshnessDriver final : public QObject {
    Q_OBJECT

public:
    using Action = void (*)();
    using Held = bool (*)();

    enum class Step {
        Stored,
        Account,
        Keys,
        StaleKeysPrompt,
        Prompt,
        PromptConflict,
        PendingSaveConflict,
        ReadExpiry,
        StorageExpiry,
        ReadyStorageExpiry,
        Closing,
        Shutdown,
        OwnerDestroyed
    };

    GrantFreshnessDriver(HostImpl *host, IModule *module);
    void run(Step step);

public slots:
    void onEnterNested();
    void onPoll();
    void onDeniedSaved(uint64_t revision, const QString &error);
    void onLateRead(uint64_t revision, const PluginAccessRequest &request);
    void onLateStorage(uint64_t revision);

protected:
    auto eventFilter(QObject *watched, QEvent *event) -> bool override;
    void promptConflict();

private:
    QPointer<HostImpl> m_host;
    IModule *m_module = nullptr;
    AppStateLock *m_lock = nullptr;
    plugin::Caller m_known;
    PluginAccessRequest m_request;
    QPointer<pluginaccess::Job> m_worker;
    QPointer<modal::PluginAccessPrompt> m_prompt;
    QEventLoop m_nested;
    QTimer m_enter;
    QTimer m_poll;
    Step m_step = Step::Stored;
    uint64_t m_job = 0;
    uint64_t m_session = 0;
    uint64_t m_generation = 0;
    int m_ticks = 0;
    bool m_save_started = false;
    bool m_denied_saved = false;
    bool m_released = false;
    bool m_late = false;
    bool m_prompt_opened = false;
    Action m_fsync_arm = nullptr;
    Held m_fsync_held = nullptr;
    Action m_fsync_release = nullptr;
};

// Run with an enabled external fixture module and an isolated, unlocked HOME.
void grantFreshnessSmoke(HostImpl *host, IModule *module);

} // namespace smoke
