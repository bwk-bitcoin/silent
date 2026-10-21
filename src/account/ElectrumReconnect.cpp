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
    connect(m_retry, &QTimer::timeout, this, &ElectrumReconnect::onRetry, qontrol::UNIQUE);
}

void ElectrumReconnect::connected() {
    m_state->retry_connected();
}

void ElectrumReconnect::stopped() {
    m_state->retry_stopped();
    m_retry->stop();
}

void ElectrumReconnect::lost() {
    auto decision = m_state->retry_lost(QDateTime::currentMSecsSinceEpoch());
    if (decision.give_up) {
        qWarning() << "electrum: disconnected" << decision.losses
                   << "times in the last minute, not reconnecting";
        m_retry->stop();
        emit gaveUp(static_cast<int>(decision.losses));
        return;
    }
    if (decision.schedule) {
        m_retry->start(static_cast<int>(decision.delay_ms));
    }
}

void ElectrumReconnect::onRetry() {
    if (m_state->retry_due()) {
        emit reconnect();
    }
}

} // namespace account
