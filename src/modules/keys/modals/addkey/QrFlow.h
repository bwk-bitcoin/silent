#pragma once

#include <QString>

#include "AddKeyFlow.h"
#include "qr/QrTypes.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class ModalStatus;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::flow {
class CameraStep;
} // namespace keys::flow

namespace qr {
class QrDisplay;
class QrDisplayEngine;
class QrEngine;
} // namespace qr

namespace keys::modal {

class KeyReview;

// Adds an airgap key over QR codes: shows the request for the key, scans the device's
// answer back, then reviews the key before adding it with the QR route.
class QrFlow final : public AddKeyFlow {
    Q_OBJECT

public:
    explicit QrFlow(KeysModule *module);
    ~QrFlow() override;
    // Shows the request and asks the device for its key.
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onQrXpub(const QString &fingerprint, const QString &path, const QString &xpub);
    void onQrError(qr::QrError kind, const QString &message);
    void onQrFrame(const qr::QrFrame &frame);

protected:
    // The view on display: Back and the primary action act on it.
    enum class View { Display, Camera, Review, Failure };

    void init();
    void doConnect();
    void view();

    // QR code view
    void initDisplay();
    void connectDisplay();
    [[nodiscard]] auto viewDisplay() -> QWidget *;
    void showDisplay();

    // Camera view
    void initCamera();
    void connectCamera();
    void showCamera();
    void hideCamera();

    // Review view
    void initReview();
    void connectReview();
    void showReview();
    void hideReview();

    // Failure view
    void initFailure();
    void connectFailure();
    void showFailure(const QString &message);
    void hideFailure();

    // Shared
    void stop();

private:
    KeysModule *m_module = nullptr;
    View m_view = View::Display;
    qr::QrEngine *m_qr_engine = nullptr;

    qr::QrDisplayEngine *m_qr_display_engine = nullptr;
    catalog::Label *m_qr_hint = nullptr;
    qr::QrDisplay *m_qr_display = nullptr;
    catalog::Button *m_qr_next = nullptr;
    catalog::ModalActions *m_qr_actions = nullptr;
    QWidget *m_qr_step = nullptr;

    flow::CameraStep *m_camera = nullptr;

    KeyReview *m_review = nullptr;

    catalog::ModalStatus *m_status = nullptr;
    catalog::Button *m_try_again = nullptr;
    catalog::ModalActions *m_status_actions = nullptr;
};

} // namespace keys::modal
