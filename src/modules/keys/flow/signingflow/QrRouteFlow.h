#pragma once

#include <QString>

#include <cstdint>

#include "modules/keys/flow/SigningFlow.h"
#include "qr/QrTypes.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class Stepper;
} // namespace catalog

namespace qr {
class QrDisplay;
class QrDisplayEngine;
class QrEngine;
struct QrFrame;
} // namespace qr

namespace keys::flow {

class CameraStep;
class VerifyResultView;

// Signs, checks an address or fetches a descriptor over QR codes: shows the request
// with its tuning controls, scans the device's answer back, and for an address shows
// what the device said about it.
class QrRouteFlow final : public SigningRouteFlow {
    Q_OBJECT

public:
    QrRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row, QString psbt);
    ~QrRouteFlow() override;
    // Shows the request.
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onQrSigned(const QString &psbt_v2_hex);
    void onQrVerified(bool matched);
    void onQrDescriptor(const QString &descriptor);
    void onQrError(qr::QrError kind, const QString &message);
    void onQrSettingsChanged();
    void onQrFrame(const qr::QrFrame &frame);

protected:
    // The view on display: Back and the primary action act on it.
    enum class View : std::uint8_t { Display, Camera, VerifyResult };

    void init();
    void doConnect();
    void view();

    // QR code view
    void initDisplay();
    void connectDisplay();
    [[nodiscard]] auto viewDisplay() -> QWidget *;
    void showDisplay();
    // The steppers' levels to the engine.
    void applyQrSettings();

    // Camera view
    void initCamera();
    void connectCamera();
    void showCamera();
    void hideCamera();

    // Verify result view
    void initVerifyResult();
    void connectVerifyResult();
    void showVerifyResult(SigningFlow::VerifyResult result);
    void hideVerifyResult();

    // Shared
    void stop();

private:
    View m_view = View::Display;
    qr::QrEngine *m_qr_engine = nullptr;

    qr::QrDisplayEngine *m_qr_display_engine = nullptr;
    catalog::Label *m_qr_hint = nullptr;
    qr::QrDisplay *m_qr_display = nullptr;
    catalog::Stepper *m_qr_contrast = nullptr;
    catalog::Stepper *m_qr_density = nullptr;
    catalog::Stepper *m_qr_speed = nullptr;
    catalog::Button *m_qr_next = nullptr;
    catalog::ModalActions *m_qr_actions = nullptr;
    QWidget *m_qr_step = nullptr;

    CameraStep *m_camera = nullptr;

    VerifyResultView *m_result = nullptr;
};

} // namespace keys::flow
