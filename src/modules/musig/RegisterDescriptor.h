#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/types.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

class MusigAccountController;
class QEvent;

namespace catalog {
class Button;
class Label;
class ModalActions;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

// Register Descriptor, from a Multikey account's Settings: one row per key of the
// wallet. On open every key on its connected USB device is asked whether it holds the
// account (nothing registered), so its row reads Registered or Not Registered. A click
// registers the account on that key's device, every device at once; a failure says why
// on its row and a click tries again. A key whose device is not connected is greyed and
// checked again when clicked; a key with no device to register on reads Not Needed.
class RegisterDescriptor final : public catalog::Modal {
    Q_OBJECT

public:
    explicit RegisterDescriptor(MusigAccountController *controller);

public slots:
    void onRowClicked();
    void onDoneClicked();
    void onRegistrationChecked(ReqId req_id, bool registered);
    void onDescriptorRegistered(ReqId req_id, const QString &fingerprint);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);

protected:
    // Where a key's device stands with the account.
    enum class State : std::uint8_t {
        NotNeeded,
        Disconnected,
        Checking,
        NotRegistered,
        Registering,
        Registered,
        Failed
    };

    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    // Checks the key's device when it is connected, else marks it Disconnected.
    void check(const QString &fingerprint);
    void refresh();
    // The key's route needs the account registered on a device (USB, QR, SD card).
    [[nodiscard]] auto needsRegister(const QString &fingerprint) const -> bool;
    [[nodiscard]] auto onDevice(const QString &fingerprint) const -> bool;

private:
    MusigAccountController *m_controller = nullptr;
    QString m_descriptor;
    QStringList m_fingerprints;
    QHash<QString, State> m_states;
    // Why a key's device failed, by fingerprint, shown on its row.
    QHash<QString, QString> m_failures;
    // Requests in flight, request id to fingerprint.
    QHash<ReqId, QString> m_requests;
    catalog::Label *m_lead = nullptr;
    qontrol::Column *m_rows = nullptr;
    catalog::Button *m_done_btn = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

} // namespace musig::modal
