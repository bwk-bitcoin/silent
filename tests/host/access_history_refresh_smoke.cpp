#include "access_history_refresh_smoke.h"

#include "AppController.h"
#include "PluginBundleFixture.h"
#include "access_async_smoke.h"
#include "catalog/Button.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "views/modals/AppSettings.h"
#include "views/modals/PluginAccessPrompt.h"
#include "views/modals/appsettings/AccessFlow.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QThread>
#include <QVariant>
#include <cstdio>
#include <silent.h>

namespace smoke {
namespace {
constexpr int READ_TIMEOUT_MS = 5000;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("access history refresh: %s", message);
    }
}

void waitForState(PluginRegistry *registry) {
    // The prompt can close before the save notification reaches the registry.
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QElapsedTimer timer;
    timer.start();
    while (registry->pluginStateLoading() && timer.elapsed() < READ_TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(!registry->pluginStateLoading(), "the async history snapshot completes");
    require(registry->pluginRemovalReadError().isEmpty(), "the history snapshot is readable");
}

auto accessButton(modal::AppSettings &settings, const QString &id) -> catalog::Button * {
    for (auto *button : settings.findChildren<catalog::Button *>(QStringLiteral("pluginAccess"))) {
        if (button->property("pluginId").toString() == id) {
            return button;
        }
    }
    return nullptr;
}

void forgetLastAnswer(modal::AppSettings &settings, PluginRegistry *registry, const QString &id,
                      const QString &hash, bool close_pending) {
    QPointer<modal::AccessFlow> flow = settings.findChild<modal::AccessFlow *>();
    require(flow != nullptr, "the real Settings access flow is open");
    catalog::Button *forget = nullptr;
    catalog::Button *done = nullptr;
    QElapsedTimer timer;
    timer.start();
    while (forget == nullptr && timer.elapsed() < READ_TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        for (auto *button : flow->findChildren<catalog::Button *>()) {
            if (button->property("accessHash").toString() == hash) {
                forget = button;
            }
            if (button->text() == TR("btn-done")) {
                done = button;
            }
        }
        QThread::msleep(1);
    }
    require(forget != nullptr && forget->isEnabled(), "the last history entry loads for Forget");
    require(done != nullptr && done->isEnabled(), "the flow has an available Done button");
    auto history = ::app_plugin_access_history(rust::String(id.toStdString()));
    require(history.is_ok && history.entries.size() == 1,
            "Forget starts with exactly one durable history entry");

    AppStateLock lock;
    if (close_pending) {
        lock.hold();
    }
    forget->click();
    require(!forget->isEnabled(), "Forget disables the row while its save is pending");
    if (close_pending) {
        done->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(flow == nullptr, "Done destroys the flow before the held save can complete");
        require(registry->pluginHasAccessHistory(id) && accessButton(settings, id) != nullptr,
                "Settings retains Access while the save is still blocked on app.lock");
        lock.release();
    } else {
        timer.restart();
        while (!forget->isEnabled() && timer.elapsed() < READ_TIMEOUT_MS) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(forget->isEnabled() && !forget->isVisibleTo(flow.data()),
                "the completed Forget hides the last entry");
        done->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(flow == nullptr, "Done destroys the completed access flow");
    }

    // No rescan or manual refresh: the save completion must update both consumers.
    timer.restart();
    while ((registry->pluginHasAccessHistory(id) || registry->pluginStateLoading() ||
            accessButton(settings, id) != nullptr) &&
           timer.elapsed() < READ_TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(!registry->pluginStateLoading() && registry->pluginRemovalReadError().isEmpty(),
            "the post-Forget registry snapshot completes without error");
    require(!registry->pluginHasAccessHistory(id),
            "Forget removes cached history without a rescan");
    require(accessButton(settings, id) == nullptr,
            "Settings removes Access after Forget and Done without a flow receiver or rescan");
    history = ::app_plugin_access_history(rust::String(id.toStdString()));
    require(history.is_ok && history.entries.empty(), "Forget durably removes the last entry");
}

void firstAnswer(PluginGrant grant, bool stale_epoch) {
    auto suffix =
        grant == PluginGrant::Allowed ? QStringLiteral("allowed") : QStringLiteral("denied");
    if (stale_epoch) {
        suffix = QStringLiteral("refused");
    }
    auto id = QStringLiteral("history.refresh.") + suffix;
    QFile fixture(QStringLiteral(SILENT_REMOVAL_FIXTURE));
    require(fixture.open(QIODevice::ReadOnly), "the native bundle fixture opens");
    auto bytes = fixture.readAll();
    bytes.append(id.toUtf8());
    auto hash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    require(installBundle(id, QStringLiteral("History refresh fixture"), bytes).is_ok,
            "a fresh plugin with no permission answers installs");
    auto *registry = AppController::get()->pluginRegistry();
    registry->rescanDiscoveredPlugins();
    waitForState(registry);
    require(!registry->pluginHasAccessHistory(id), "the initial snapshot has no access history");

    modal::AppSettings settings;
    settings.show();
    require(accessButton(settings, id) == nullptr, "Settings initially has no Access action");
    auto request = ::app_plugin_access_request(rust::String(id.toStdString()),
                                               rust::String(hash.toStdString()));
    require(request.is_ok && request.access.state == PluginGrant::Unasked,
            "the first prompt starts with an Unasked state grant");
    modal::PluginAccessPrompt prompt(
        QStringLiteral("History refresh fixture"), QStringLiteral("1.0.0"), request.access,
        request.storage_epoch + (stale_epoch ? 1 : 0), request.grant_version, {.state = true});
    if (grant == PluginGrant::Allowed) {
        prompt.onAllowClicked();
    } else {
        prompt.onDenyClicked();
    }
    prompt.exec();
    require(prompt.saved() == !stale_epoch, "the prompt reports the actual save outcome");
    require(prompt.error().isEmpty() == !stale_epoch, "a refused save retains its error");

    // Only the save publication may refresh Settings after answering the prompt.
    waitForState(registry);
    auto answers =
        ::app_plugin_answers(rust::String(id.toStdString()), rust::String(hash.toStdString()));
    require(answers.state == (stale_epoch ? PluginGrant::Unasked : grant),
            "the durable answer matches the accepted or refused prompt save");
    require(registry->pluginHasAccessHistory(id) == !stale_epoch,
            "publication refreshes history without inventing history on a refused save");
    auto *button = accessButton(settings, id);
    require((button != nullptr) == !stale_epoch,
            "the actual Settings Access action follows the first prompt without a rescan");
    if (button != nullptr) {
        require(button->isEnabled() && !button->isHidden(), "the new Access action is available");
        button->click();
        require(settings.findChild<modal::AccessFlow *>() != nullptr,
                "clicking the new Settings Access action opens the real access flow");
        forgetLastAnswer(settings, registry, id, hash, grant == PluginGrant::Denied);
    }
}
} // namespace

void accessHistoryRefreshSmoke() {
    firstAnswer(PluginGrant::Allowed, false);
    firstAnswer(PluginGrant::Denied, false);
    firstAnswer(PluginGrant::Denied, true);
    std::fprintf(stdout, "First-prompt Settings Access: Allow, Deny and refused save OK\n");
    std::fprintf(stdout,
                 "Last-entry Forget: Done after save and flow destroyed during held save OK\n");
}

} // namespace smoke
