#include "grant_freshness_smoke.h"

#include "access_async_smoke.h"
#include "host/CallerHandles.h"
#include "interfaces/host.h"
#include "views/modals/PluginAccessPrompt.h"

#include <QApplication>
#include <QEvent>
#include <QHash>
#include <QSet>
#include <common.h>
#include <cstdio>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#define private public
#define protected public
#include "host/Host.h"
#undef protected
#undef private

namespace smoke {
namespace {
constexpr int EXPIRY_MS = 10;
constexpr int MAX_TICKS = 5000;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("grant freshness: %s", message);
    }
}

auto openPrompt() -> modal::PluginAccessPrompt * {
    for (auto *widget : QApplication::topLevelWidgets()) {
        auto *prompt = qobject_cast<modal::PluginAccessPrompt *>(widget);
        if (prompt != nullptr && prompt->isVisible()) {
            return prompt;
        }
    }
    return nullptr;
}
} // namespace

GrantFreshnessDriver::GrantFreshnessDriver(HostImpl *host, IModule *module)
    : m_host(host),
      m_module(module),
      m_known(HostImpl::caller(module)) {
    require(m_known.external && !m_known.pluginId.isEmpty(), "the fixture caller is external");
    m_enter.setSingleShot(true);
    m_poll.setInterval(1);
    connect(&m_enter, &QTimer::timeout, this, &GrantFreshnessDriver::onEnterNested,
            qontrol::UNIQUE);
    connect(&m_poll, &QTimer::timeout, this, &GrantFreshnessDriver::onPoll, qontrol::UNIQUE);
}

void GrantFreshnessDriver::run(Step step) {
    m_step = step;
    m_request = pluginaccess::request(m_known.pluginId, m_known.pluginHash);
    require(m_request.is_ok, "the fixture reads its epoch");
    m_request.access.state = step == Step::Prompt ? PluginGrant::Unasked : PluginGrant::Allowed;
    m_request.access.account = PluginGrant::Allowed;
    m_request.access.keys = PluginGrant::Allowed;
    if (step == Step::StaleKeysPrompt || step == Step::PromptConflict ||
        step == Step::PendingSaveConflict) {
        m_request.access.keys = PluginGrant::Unasked;
    }
    require(pluginaccess::save(m_request.access, m_request.storage_epoch).isEmpty(),
            "the initial grant is published");
    if (step == Step::PromptConflict || step == Step::PendingSaveConflict) {
        promptConflict();
        return;
    }
    AppStateLock lock;
    m_lock = &lock;
    if (step != Step::Prompt) {
        lock.hold();
    }
    qApp->installEventFilter(this);
    m_enter.start(1);
    switch (step) {
    case Step::Account:
    case Step::Keys: {
        auto granted = m_host->allowed(
            m_module, step == Step::Account ? host::Grant::Account : host::Grant::Keys, false);
        require(m_denied_saved, "revocation completes before the capability check resumes");
        require(!granted, "a completed denial fences the real Account/Keys permission path");
        break;
    }
    case Step::Stored:
    case Step::Prompt: {
        auto answer = m_host->requestAccess(m_module, {.state = step == Step::Prompt});
        require(m_denied_saved, "denial completed inside the deeper loop before consumption");
        require(!answer.state, "an old Allowed result cannot survive the published denial");
        break;
    }
    case Step::StaleKeysPrompt: {
        auto answer = m_host->requestAccess(m_module, {.keys = true});
        m_poll.stop();
        require(m_denied_saved, "Account denial publishes before the Keys request resumes");
        require(!m_prompt_opened, "a stale Keys-only snapshot never opens an access prompt");
        require(!answer.account && !answer.keys && !answer.state,
                "the stale Keys-only request grants nothing");
        auto current = pluginaccess::request(m_known.pluginId, m_known.pluginHash);
        require(current.is_ok && current.access.account == PluginGrant::Denied &&
                    current.access.keys == PluginGrant::Unasked,
                "refusing the stale prompt keeps Account Denied and Keys Unasked");
        break;
    }
    case Step::PromptConflict:
    case Step::PendingSaveConflict:
        qFatal("grant freshness: prompt conflicts use their own modal wait");
        break;
    case Step::ReadExpiry: {
        auto answer = m_host->readGrantRequest(m_module, m_known);
        require(!answer.has_value(), "the expired read refuses its late result");
        break;
    }
    case Step::StorageExpiry:
    case Step::ReadyStorageExpiry:
    case Step::Closing:
    case Step::Shutdown:
    case Step::OwnerDestroyed: {
        auto answer = m_host->openStorageSession(m_module, m_known);
        require(!answer.has_value(), "an abandoned storage wait never hands out a session");
        break;
    }
    }
    qApp->removeEventFilter(this);
    if (m_host != nullptr) {
        require(m_host->m_grant_waits.isEmpty(), "all stack loop pointers were removed");
        require(m_host->m_grant_requests.isEmpty() && m_host->m_storage_sessions.isEmpty(),
                "abandoned answers do not remain in the host");
    } else {
        require(step == Step::OwnerDestroyed, "only the owner-destruction case deletes the host");
    }
    if (step == Step::Closing) {
        m_host->setCallerClosing(m_module, false);
    }
    m_lock = nullptr;
}

auto GrantFreshnessDriver::eventFilter(QObject *watched, QEvent *event) -> bool {
    if (event->type() == QEvent::Show &&
        qobject_cast<modal::PluginAccessPrompt *>(watched) != nullptr) {
        m_prompt_opened = true;
    }
    return QObject::eventFilter(watched, event);
}

void GrantFreshnessDriver::promptConflict() {
    m_request = pluginaccess::request(m_known.pluginId, m_known.pluginHash);
    require(m_request.is_ok && m_request.access.account == PluginGrant::Allowed &&
                m_request.access.keys == PluginGrant::Unasked,
            "the Keys-only prompt captures Account Allowed and Keys Unasked");
    if (m_step == Step::PendingSaveConflict) {
        m_fsync_arm = reinterpret_cast<Action>(dlsym(RTLD_DEFAULT, "silentTestAppStateFsyncArm"));
        m_fsync_held = reinterpret_cast<Held>(dlsym(RTLD_DEFAULT, "silentTestFsyncHeld"));
        m_fsync_release = reinterpret_cast<Action>(dlsym(RTLD_DEFAULT, "silentTestFsyncRelease"));
        require(m_fsync_arm != nullptr && m_fsync_held != nullptr && m_fsync_release != nullptr,
                "--grant-pending-conflict requires LD_PRELOAD=libfsync_hold_shim.so");
    }
    auto *prompt = new modal::PluginAccessPrompt(
        QStringLiteral("Grant freshness fixture"), QStringLiteral("1"), m_request.access,
        m_request.storage_epoch, m_request.grant_version, {.keys = true});
    m_prompt = prompt;
    m_poll.start();
    m_host->execAccessModal(m_module, prompt);
    m_poll.stop();
    require(m_denied_saved, "the intervening Account denial completed before prompt completion");
    require(prompt->answered() && !prompt->saved(), "the stale Allow is explicitly refused");
    require(prompt->error() == QStringLiteral("Access answers changed since they were read: "
                                              "these conflicting answers were not saved"),
            "the real prompt receives the explicit revision-conflict response");
    auto current = pluginaccess::request(m_known.pluginId, m_known.pluginHash);
    require(current.is_ok && current.access.account == PluginGrant::Denied &&
                current.access.keys == PluginGrant::Unasked &&
                current.access.state == PluginGrant::Allowed,
            "the stale prompt saves no fields and never restores Account Allowed");
    delete prompt;
}

void GrantFreshnessDriver::onEnterNested() {
    if (m_step == Step::Prompt) {
        auto *prompt = openPrompt();
        if (prompt == nullptr) {
            require(++m_ticks < MAX_TICKS, "the access prompt opens");
            m_enter.start(1);
            return;
        }
        m_prompt = prompt;
        prompt->onAllowClicked();
    } else {
        require(!m_host->m_grant_waits.isEmpty(), "outer loop A is waiting before loop B starts");
        m_job = m_host->m_grant_job;
        auto wait = m_host->m_grant_waits.value(m_job);
        m_worker = wait.worker;
        require(m_worker != nullptr, "the outer wait owns its worker");
        connect(m_worker, &pluginaccess::Job::requestRead, this, &GrantFreshnessDriver::onLateRead,
                qontrol::UNIQUE);
        connect(m_worker, &pluginaccess::Job::storageOpened, this,
                &GrantFreshnessDriver::onLateStorage, qontrol::UNIQUE);
        switch (m_step) {
        case Step::ReadExpiry:
        case Step::StorageExpiry:
            wait.deadline->start(EXPIRY_MS);
            break;
        case Step::Closing:
            m_host->setCallerClosing(m_module, true);
            break;
        case Step::Shutdown:
            m_host->onAbandonGrantWaits();
            break;
        case Step::OwnerDestroyed:
            delete m_host.data();
            break;
        default:
            m_lock->release();
            m_released = true;
            break;
        }
    }
    m_poll.start();
    m_nested.exec();
    if (m_step != Step::StaleKeysPrompt) {
        m_poll.stop();
    }
}

void GrantFreshnessDriver::onPoll() {
    require(++m_ticks < MAX_TICKS, "the deeper loop finishes without hanging");
    if (m_step == Step::PromptConflict || m_step == Step::PendingSaveConflict) {
        require(m_prompt != nullptr, "the conflict test owns its prompt");
        if (!m_prompt->isVisible()) {
            return;
        }
        require(m_step != Step::PendingSaveConflict || !m_denied_saved || m_released,
                "the denial must hit the app-state fsync hold before completing");
        if (!m_save_started) {
            m_save_started = true;
            m_request.access.account = PluginGrant::Denied;
            if (m_step == Step::PendingSaveConflict) {
                m_fsync_arm();
            }
            auto *save = new pluginaccess::Job;
            connect(save, &pluginaccess::Job::saved, this, &GrantFreshnessDriver::onDeniedSaved,
                    qontrol::UNIQUE);
            save->save(0, m_request.access, m_request.storage_epoch);
        } else if (m_step == Step::PendingSaveConflict && !m_released && m_fsync_held()) {
            require(!m_denied_saved && !m_prompt->answered(),
                    "denial owns the publication lock before the prompt starts saving");
            m_prompt->onAllowClicked();
            require(m_prompt->answered() && !m_prompt->saved() && m_prompt->error().isEmpty(),
                    "Allow queued an unsaved prompt answer while denial publication is held");
            m_released = true;
            m_fsync_release();
            // Keep the prompt's loop suspended until the denial completion is delivered too.
            m_nested.exec();
        }
        return;
    }
    if (m_step == Step::StaleKeysPrompt && m_denied_saved) {
        if (auto *prompt = openPrompt(); prompt != nullptr) {
            prompt->reject();
        }
        return;
    }
    if (m_host == nullptr) {
        if (!m_released) {
            m_lock->release();
            m_released = true;
        }
        if (m_late) {
            m_nested.quit();
        }
        return;
    }
    if (m_step == Step::Stored || m_step == Step::Account || m_step == Step::Keys ||
        m_step == Step::Prompt || m_step == Step::StaleKeysPrompt) {
        if (m_step == Step::Prompt && m_prompt != nullptr && !m_prompt->error().isEmpty()) {
            qFatal("grant freshness: expected successful prompt save: %s",
                   qPrintable(m_prompt->error()));
        }
        auto ready = m_step == Step::Prompt ? m_prompt != nullptr && m_prompt->saved()
                                            : m_host->m_grant_requests.contains(m_job);
        if (!ready || m_save_started) {
            return;
        }
        if (m_step != Step::Prompt) {
            auto snapshot = m_host->m_grant_requests.value(m_job);
            require(snapshot.is_ok && snapshot.access.state == PluginGrant::Allowed,
                    "A's Allowed result was delivered while A is suspended under B");
            if (m_step == Step::StaleKeysPrompt) {
                require(snapshot.access.account == PluginGrant::Allowed &&
                            snapshot.access.keys == PluginGrant::Unasked,
                        "A's Keys-only snapshot predates the Account denial under B");
            }
        }
        m_save_started = true;
        m_request.access.account = PluginGrant::Denied;
        if (m_step != Step::StaleKeysPrompt) {
            m_request.access.state = PluginGrant::Denied;
            m_request.access.keys = PluginGrant::Denied;
        }
        auto *save = new pluginaccess::Job;
        connect(save, &pluginaccess::Job::saved, this, &GrantFreshnessDriver::onDeniedSaved,
                qontrol::UNIQUE);
        save->save(0, m_request.access, m_request.storage_epoch);
        return;
    }
    if (m_step == Step::ReadyStorageExpiry && m_session == 0) {
        if (!m_host->m_storage_sessions.contains(m_job)) {
            return;
        }
        m_session = m_host->m_storage_sessions.value(m_job);
        require(m_session != 0, "the session was opened before expiry inside B");
        m_generation = ::storage_session_generation(m_session);
        m_host->m_grant_waits.value(m_job).deadline->start(EXPIRY_MS);
    }
    if (m_host->m_grant_waits.contains(m_job)) {
        return;
    }
    require(!m_host->m_grant_requests.contains(m_job) &&
                !m_host->m_storage_sessions.contains(m_job),
            "expiry or closing removed answers before A can unwind B");
    if (m_step == Step::ReadyStorageExpiry) {
        require(::storage_delivery_error(m_session, m_generation) == "revoked",
                "expiry revokes an already delivered session immediately inside B");
        m_nested.quit();
        return;
    }
    if (!m_released) {
        m_lock->release();
        m_released = true;
    }
    if (m_late) {
        m_nested.quit();
    }
}

void GrantFreshnessDriver::onDeniedSaved([[maybe_unused]] uint64_t revision, const QString &error) {
    require(error.isEmpty(), "Settings-like denial saves successfully inside B");
    m_denied_saved = true;
    if (m_step == Step::PromptConflict) {
        require(m_prompt != nullptr && m_prompt->isVisible() && !m_prompt->answered(),
                "denial publishes while the old Keys-only prompt remains unanswered");
        m_prompt->onAllowClicked();
        return;
    }
    if (m_step == Step::PendingSaveConflict) {
        m_nested.quit();
        return;
    }
    m_nested.quit();
}

void GrantFreshnessDriver::onLateRead([[maybe_unused]] uint64_t revision,
                                      [[maybe_unused]] const PluginAccessRequest &request) {
    m_late = true;
}

void GrantFreshnessDriver::onLateStorage([[maybe_unused]] uint64_t revision) {
    if (m_step == Step::ReadyStorageExpiry) {
        return;
    }
    require(m_worker != nullptr && m_worker->takeSession() == 0,
            "a late storage result cannot be taken after abandonment");
    m_late = true;
}

void grantFreshnessSmoke(HostImpl *host, IModule *module) {
    for (auto [step, name] :
         {std::pair{GrantFreshnessDriver::Step::Stored, "Stored"},
          std::pair{GrantFreshnessDriver::Step::Account, "Account"},
          std::pair{GrantFreshnessDriver::Step::Keys, "Keys"},
          std::pair{GrantFreshnessDriver::Step::StaleKeysPrompt, "StaleKeysPrompt"},
          std::pair{GrantFreshnessDriver::Step::Prompt, "Prompt"},
          std::pair{GrantFreshnessDriver::Step::PromptConflict, "PromptConflict"},
          std::pair{GrantFreshnessDriver::Step::ReadExpiry, "ReadExpiry"},
          std::pair{GrantFreshnessDriver::Step::StorageExpiry, "StorageExpiry"},
          std::pair{GrantFreshnessDriver::Step::ReadyStorageExpiry, "ReadyStorageExpiry"},
          std::pair{GrantFreshnessDriver::Step::Closing, "Closing"}}) {
        GrantFreshnessDriver driver(host, module);
        driver.run(step);
        std::fprintf(stdout, "Integrated grant freshness %s: OK\n", name);
    }
    if (QApplication::arguments().contains(QStringLiteral("--grant-pending-conflict"))) {
        GrantFreshnessDriver pending(host, module);
        pending.run(GrantFreshnessDriver::Step::PendingSaveConflict);
        std::fprintf(stdout, "Integrated grant freshness PendingSaveConflict: OK\n");
    }
    HostImpl quittingHost;
    GrantFreshnessDriver shutdown(&quittingHost, module);
    shutdown.run(GrantFreshnessDriver::Step::Shutdown);
    std::fprintf(stdout, "Integrated grant freshness Shutdown: OK\n");
    GrantFreshnessDriver destroyed(new HostImpl, module);
    destroyed.run(GrantFreshnessDriver::Step::OwnerDestroyed);
    std::fprintf(stdout, "Integrated grant freshness OwnerDestroyed: OK\n");
}

} // namespace smoke
