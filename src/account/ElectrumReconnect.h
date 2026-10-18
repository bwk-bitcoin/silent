#pragma once

#include <QList>
#include <QObject>

class QTimer;

namespace account {

// Reconnects an account's Electrum connection that dropped on its own: armed once it
// connects, disarmed when the user or the account stops it. A lost connection asks
// for a reconnect shortly after, unless it dropped more than MAX_LOSSES times within
// the last LOSS_WINDOW_MS: then it stays down until started again.
class ElectrumReconnect final : public QObject {
    Q_OBJECT

public:
    explicit ElectrumReconnect(QObject *parent = nullptr);

    // The connection is up: a later loss reconnects.
    void connected();
    // Stopped on purpose: a later loss does not reconnect.
    void stopped();
    // The connection dropped, or failed while it ran.
    void lost();

signals:
    // Start the connection again.
    void reconnect();

public slots:
    void onRetry();

protected:
    static constexpr int MAX_LOSSES = 3;
    static constexpr qint64 LOSS_WINDOW_MS = 60 * 1000;
    static constexpr int RETRY_DELAY_MS = 2000;

private:
    bool m_armed = false;
    // When each recent loss happened, ms since the epoch.
    QList<qint64> m_losses;
    QTimer *m_retry = nullptr;
};

} // namespace account
