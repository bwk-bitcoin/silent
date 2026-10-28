#pragma once

#include "host/PluginRegistry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QThread>
#include <common.h>
#include <utility>

namespace smoke {

constexpr int PLUGIN_WAIT_TIMEOUT_MS = 10000;

class PluginStopProbe final : public QObject {
    Q_OBJECT

public:
    PluginStopProbe(PluginRegistry *registry, QString id) : m_id(std::move(id)) {
        connect(registry, &PluginRegistry::pluginStopFinished, this, &PluginStopProbe::onFinished,
                qontrol::UNIQUE);
    }

    auto wait() -> QString {
        QElapsedTimer timer;
        timer.start();
        while (!m_finished && timer.elapsed() < PLUGIN_WAIT_TIMEOUT_MS) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return m_finished ? m_error : QStringLiteral("Plugin stop completion timed out");
    }

public slots:

    void onFinished(const QString &id, [[maybe_unused]] bool removed, const QString &error) {
        if (id != m_id) {
            return;
        }
        m_finished = true;
        m_error = error;
    }

private:
    QString m_id;
    QString m_error;
    bool m_finished = false;
};

inline void waitForPluginScan(PluginRegistry *registry) {
    QElapsedTimer timer;
    timer.start();
    while (registry->pluginStateLoading() && timer.elapsed() < PLUGIN_WAIT_TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (registry->pluginStateLoading() || !registry->pluginRemovalReadError().isEmpty()) {
        qFatal("checked plugin scan did not complete: %s",
               qPrintable(registry->pluginRemovalReadError()));
    }
}

inline void rescanPluginsAndWait(PluginRegistry *registry) {
    registry->rescanDiscoveredPlugins();
    waitForPluginScan(registry);
}

inline auto enablePluginAndWait(PluginRegistry *registry, const QString &id) -> bool {
    waitForPluginScan(registry);
    if (!registry->setEnabled(id, true)) {
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while ((registry->pluginEnablePending(id) || registry->pluginStopPending(id) ||
            registry->pluginStateLoading()) &&
           timer.elapsed() < PLUGIN_WAIT_TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (registry->pluginEnablePending(id) || registry->pluginStopPending(id) ||
        registry->pluginStateLoading()) {
        qFatal("plugin enable/compensation did not complete: %s", qPrintable(id));
    }
    return registry->isPluginEnabled(id);
}

inline auto stopPlugin(PluginRegistry *registry, const QString &id, bool remove = false)
    -> QString {
    PluginStopProbe probe(registry, id);
    if (remove) {
        registry->removePlugin(id);
    } else if (!registry->setEnabled(id, false)) {
        return QStringLiteral("Plugin stop was refused");
    }
    return probe.wait();
}

} // namespace smoke
