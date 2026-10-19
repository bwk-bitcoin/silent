#pragma once

#include "catalog/containers/Modal.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <cstdint>
#include <silent.h>

class QEvent;

namespace catalog {
class Button;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
} // namespace catalog

namespace qontrol {
class Column;
}

namespace state {

class UnlockDevices;

// Unlocks the app state with its signing device. Nothing on disk names the device:
// every ready device in sight is asked for its key at the path the check file carries,
// all at once (one waiting on its user does not hold the others), until one key opens
// the state. Accepted once unlocked; closing it leaves the app.
class DeviceUnlock final : public catalog::Modal {
    Q_OBJECT

public:
    // Devices are asked on mainnet; `network` is for tests against devices running a
    // test network app.
    explicit DeviceUnlock(const StateUnlockInfo &info, Network network = Network::Bitcoin);

public slots:
    void onDevicesChanged();
    void onXpubFetched(quint64 req_id, const QString &xpub);
    void onXpubFailed(quint64 req_id, const QString &error);
    void onTryAgainClicked();

protected:
    enum class View : std::uint8_t { Waiting, Busy };

    void init();
    void doConnect();
    void view();

    // Waiting view
    void initWaiting();
    [[nodiscard]] auto viewWaiting() -> QWidget *;
    void showWaiting();
    void rebuildDevices();
    // Asks every ready device not asked yet; back to waiting when none is pending.
    void askAll();
    // A device answered without opening the state: `error` shown once none is pending.
    void settle(const QString &error);

    // Busy view
    void initBusy();
    void showBusy();

    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    View m_view = View::Waiting;
    QString m_path;
    UnlockDevices *m_devices = nullptr;
    // The requests waiting, to the name of the device asked.
    QHash<quint64, QString> m_pending;
    // Devices asked already, by id: a key that did not open is not asked again until
    // Try Again.
    QSet<QString> m_asked;

    QWidget *m_waiting = nullptr;
    catalog::Label *m_text = nullptr;
    qontrol::Column *m_device_rows = nullptr;
    catalog::ModalStatus *m_no_device = nullptr;
    catalog::Notice *m_error = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_try_again = nullptr;

    catalog::ModalStatus *m_busy = nullptr;
};

} // namespace state
