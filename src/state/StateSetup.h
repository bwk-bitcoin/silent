#pragma once

#include "catalog/containers/Modal.h"

#include <QString>
#include <cstdint>
#include <silent.h>

class QEvent;

namespace catalog {
class Button;
class FieldGroup;
class FieldNote;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
class SelectRow;
} // namespace catalog

namespace qontrol {
class Column;
}

namespace state {

class UnlockDevices;

// The first start: picks what unlocks the app state, a signing device or a password,
// and creates it. Accepted once the state is created and unlocked; closing it leaves
// the app.
class StateSetup final : public catalog::Modal {
    Q_OBJECT

public:
    // Devices are asked on mainnet; `network` is for tests against devices running a
    // test network app.
    explicit StateSetup(Network network = Network::Bitcoin);
    ~StateSetup() override;
    // Ignored while the password is being sealed: the worker answers to this window.
    void reject() override;

public slots:
    void onDeviceChosen();
    void onPasswordChosen();
    void onBackClicked();
    void onDevicesChanged();
    void onDeviceRowClicked();
    void onDeviceContinueClicked();
    void onXpubFetched(quint64 req_id, const QString &xpub);
    void onXpubFailed(quint64 req_id, const QString &error);
    void onPasswordEdited();
    void onCreatePasswordClicked();
    void onPasswordCreated(const QString &error);

protected:
    enum class View : std::uint8_t { Choice, Device, Password, Busy };

    void init();
    void doConnect();
    void view();

    // Choice view
    void initChoice();
    [[nodiscard]] auto viewChoice() -> QWidget *;
    void showChoice();

    // Device view
    void initDevice();
    [[nodiscard]] auto viewDevice() -> QWidget *;
    void showDevice();
    void rebuildDevices();
    void updateDeviceActions();

    // Password view
    void initPassword();
    [[nodiscard]] auto viewPassword() -> QWidget *;
    void showPassword();

    // Busy view
    void initBusy();
    void showBusy(const QString &title, const QString &subtitle);

    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    View m_view = View::Choice;

    QWidget *m_choice = nullptr;
    catalog::Label *m_choice_text = nullptr;
    catalog::SelectRow *m_device_choice = nullptr;
    catalog::SelectRow *m_password_choice = nullptr;

    QWidget *m_device = nullptr;
    catalog::Label *m_device_text = nullptr;
    Network m_network = Network::Bitcoin;
    qontrol::Column *m_device_rows = nullptr;
    catalog::ModalStatus *m_no_device = nullptr;
    catalog::Notice *m_device_error = nullptr;
    catalog::ModalActions *m_device_actions = nullptr;
    catalog::Button *m_device_continue = nullptr;
    UnlockDevices *m_devices = nullptr;
    QString m_selected_id;
    QString m_selected_name;
    quint32 m_index = 0;
    quint64 m_xpub_req = 0;

    QWidget *m_password = nullptr;
    catalog::Label *m_password_text = nullptr;
    catalog::FieldGroup *m_password_fields = nullptr;
    catalog::Input *m_password_input = nullptr;
    catalog::Input *m_confirm_input = nullptr;
    catalog::FieldNote *m_password_short = nullptr;
    catalog::FieldNote *m_password_mismatch = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    catalog::Button *m_create = nullptr;

    catalog::ModalStatus *m_busy = nullptr;
    bool m_sealing = false;
};

} // namespace state
