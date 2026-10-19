#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <silent.h>

class QThread;
class QTimer;

namespace state {

// The signing devices the unlock screens see, before any module runs: a device
// service of its own, polled while the screen is up and shut down with it.
class UnlockDevices final : public QObject {
    Q_OBJECT

public:
    explicit UnlockDevices(Network network, QObject *parent = nullptr);
    ~UnlockDevices() override;

    // Every device in sight, in whatever state.
    [[nodiscard]] auto devices() const -> QList<HwiDevice>;
    // Asks `device_id` for its xpub at `path`. Answers xpubFetched() or xpubFailed()
    // with the id returned, 0 when the request could not be sent.
    auto fetchXpub(const QString &device_id, const QString &path) -> quint64;

signals:
    void devicesChanged();
    void xpubFetched(quint64 req_id, QString xpub);
    void xpubFailed(quint64 req_id, QString error);
    // From the notification thread to the GUI thread.
    void hwiEventReceived(HwiEvent event);

private slots:
    void onHwiEventReceived(const HwiEvent &event);
    void onPollDevices();

private:
    rust::Box<HwiService> m_service;
    QThread *m_thread = nullptr;
    QTimer *m_poll = nullptr;
    QList<HwiDevice> m_devices;
    quint64 m_next_req_id = 0;
};

} // namespace state
