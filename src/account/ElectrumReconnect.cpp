#include "ElectrumReconnect.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>
#include <Qontrol>

#include <common.h>

namespace account {

ElectrumReconnect::ElectrumReconnect(QObject *parent) : QObject(parent) {
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    m_retry->setInterval(RETRY_DELAY_MS);
    connect(m_retry, &QTimer::timeout, this, &ElectrumReconnect::onRetry, qontrol::UNIQUE);
}

void ElectrumReconnect::connected() {
    m_armed = true;
}

void ElectrumReconnect::stopped() {
    m_armed = false;
    m_retry->stop();
}

void ElectrumReconnect::lost() {
    if (!m_armed) {
        return;
    }
    auto now = QDateTime::currentMSecsSinceEpoch();
    m_losses.removeIf([now](qint64 at) { return now - at > LOSS_WINDOW_MS; });
    m_losses.append(now);
    if (m_losses.size() > MAX_LOSSES) {
        // Dropping again and again: left down until started again.
        qWarning() << "electrum: disconnected" << m_losses.size()
                   << "times in the last minute, not reconnecting";
        stopped();
        return;
    }
    m_retry->start();
}

void ElectrumReconnect::onRetry() {
    if (m_armed) {
        emit reconnect();
    }
}

} // namespace account
