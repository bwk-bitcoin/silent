#pragma once

#include "catalog/containers/Modal.h"

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
class SelectRow;
} // namespace catalog

namespace state {

class UnlockDevices;

// Unlocks the app state with its signing device: names the device to plug, asks it
// for the key at the stored path as soon as it is ready, and unlocks with it.
// Accepted once unlocked; closing it leaves the app.
class DeviceUnlock final : public catalog::Modal {
    Q_OBJECT

public:
    explicit DeviceUnlock(const StateUnlockInfo &info);

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
    void updateDevice();

    // Busy view
    void initBusy();
    void showBusy();

    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    View m_view = View::Waiting;
    QString m_fingerprint;
    QString m_alias;
    QString m_path;
    UnlockDevices *m_devices = nullptr;
    quint64 m_xpub_req = 0;
    // A failure stops the automatic request until Try Again.
    bool m_held = false;

    QWidget *m_waiting = nullptr;
    catalog::Label *m_text = nullptr;
    catalog::SelectRow *m_device = nullptr;
    catalog::Notice *m_error = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_try_again = nullptr;

    catalog::ModalStatus *m_busy = nullptr;
};

} // namespace state
