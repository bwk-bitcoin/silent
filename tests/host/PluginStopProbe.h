#pragma once

#include "host/PluginRegistry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <common.h>
#include <utility>

namespace smoke {

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
        while (!m_finished && timer.elapsed() < TIMEOUT_MS) {
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
    static constexpr int TIMEOUT_MS = 10000;
    QString m_id;
    QString m_error;
    bool m_finished = false;
};

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
