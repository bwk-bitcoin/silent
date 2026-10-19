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
    m_up = true;
    m_retrying = false;
}

void ElectrumReconnect::stopped() {
    m_armed = false;
    m_up = false;
    m_retrying = false;
    m_retry->stop();
}

void ElectrumReconnect::lost() {
    // Another notification of a loss already counted.
    if (!m_armed || (!m_up && !m_retrying)) {
        return;
    }
    m_up = false;
    m_retrying = false;
    auto now = QDateTime::currentMSecsSinceEpoch();
    m_losses.removeIf([now](qint64 at) { return now - at > LOSS_WINDOW_MS; });
    m_losses.append(now);
    if (m_losses.size() > MAX_LOSSES) {
        // Dropping again and again: left down until started again.
        qWarning() << "electrum: disconnected" << m_losses.size()
                   << "times in the last minute, not reconnecting";
        auto losses = static_cast<int>(m_losses.size());
        m_losses.clear();
        stopped();
        emit gaveUp(losses);
        return;
    }
    m_retry->start();
}

void ElectrumReconnect::onRetry() {
    if (m_armed) {
        m_retrying = true;
        emit reconnect();
    }
}

} // namespace account
