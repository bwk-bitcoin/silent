#pragma once

#include <optional>

#include <QPointer>
#include <QString>

#include <cstdint>

#include "modules/keys/flow/SigningFlow.h"

#include <interfaces/types.h>

class ISigner;

namespace catalog {
class Button;
class Card;
class ChunkedAddress;
class ModalActions;
class ModalStatus;
} // namespace catalog

namespace keys::flow {

class VerifyResultView;

// Signs or checks an address on a connected USB device: a spinner while the device
// waits for the user, then why it failed with Back to the other signers and Try
// Again, or for an address what the device said about it.
class UsbRouteFlow final : public SigningRouteFlow {
    Q_OBJECT

public:
    enum class UsbError : std::uint8_t { Rejected, Disconnected, Failed };

    UsbRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row, QString psbt);
    ~UsbRouteFlow() override;
    // Asks the device, found again by fingerprint when it came back with a new id.
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onDeviceSigned(ReqId req_id, const QString &psbt_base64);
    void onDeviceXpub(ReqId req_id, const QString &xpub);
    void onDeviceAddressDisplayed(ReqId req_id, const QString &address);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onInstanceRemoved(const QString &id);

protected:
    // The view on display: Back and the primary action act on it.
    enum class View : std::uint8_t { Usb, VerifyResult };

    void init();
    void doConnect();
    void view();

    // USB view
    void initUsb();
    void connectUsb();
    [[nodiscard]] auto viewUsb() -> QWidget *;
    void showUsb();
    void showUsbError(UsbError error);
    void hideUsb();
    void releaseDevice();

    // Verify result view
    void initVerifyResult();
    void connectVerifyResult();
    void showVerifyResult(SigningFlow::VerifyResult result);
    void hideVerifyResult();

private:
    View m_view = View::Usb;
    QPointer<ISigner> m_device;
    QString m_device_instance;
    ReqId m_device_req = 0;

    QWidget *m_usb_step = nullptr;
    catalog::ModalStatus *m_usb_status = nullptr;
    catalog::Card *m_usb_address_card = nullptr;
    catalog::ChunkedAddress *m_usb_address = nullptr;
    catalog::ModalActions *m_usb_actions = nullptr;
    catalog::Button *m_usb_retry = nullptr;

    VerifyResultView *m_result = nullptr;
};

} // namespace keys::flow
