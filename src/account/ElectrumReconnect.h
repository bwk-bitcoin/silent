#pragma once

#include <QObject>
#include <silent.h>

class QTimer;

namespace account {

// Reconnects an account's Electrum connection that dropped on its own: armed once it
// connects, disarmed when the user or the account stops it. Rust owns the retry
// window and budget; Qt owns the timer and delivers the reconnect request.
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

private:
    rust::Box<EndpointRetry> m_state = new_endpoint_retry();
    QTimer *m_retry = nullptr;
};

} // namespace account
