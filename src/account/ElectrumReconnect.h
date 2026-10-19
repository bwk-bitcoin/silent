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
    // The connection dropped, or failed while it ran, or a reconnect attempt failed.
    // One loss gives several notifications (the merkle client, the transport error,
    // the listener's stop): only the first of them counts.
    void lost();

signals:
    // Start the connection again.
    void reconnect();
    // More than MAX_LOSSES losses within LOSS_WINDOW_MS: no more reconnects.
    void gaveUp(int losses);

public slots:
    void onRetry();

protected:
    static constexpr int MAX_LOSSES = 3;
    static constexpr qint64 LOSS_WINDOW_MS = 60 * 1000;
    static constexpr int RETRY_DELAY_MS = 2000;

private:
    bool m_armed = false;
    // Connected since the last loss.
    bool m_up = false;
    // A reconnect was asked and has not connected yet: its failure is a loss.
    bool m_retrying = false;
    // When each recent loss happened, ms since the epoch.
    QList<qint64> m_losses;
    QTimer *m_retry = nullptr;
};

} // namespace account
