#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include "qr/QrCamera.h"
#include "qr/QrTypes.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
class ScanProgress;
} // namespace catalog

namespace qr {
class QrCameraDisplay;
class QrCameraEngine;
} // namespace qr

namespace keys::flow {

class CameraSettings;

// Reads an animated QR answer back off a device's screen: a camera's frames go through
// the scan engine, which shows them here and hands the payloads it joins to whoever
// connects to engine(). Shows the camera's state, the parts counted, and why a scanned
// answer was refused.
class CameraStep : public QWidget {
    Q_OBJECT

public:
    explicit CameraStep(QWidget *parent = nullptr);
    void setHint(const QString &hint);
    // Opens the camera picked last, from a clean scan.
    void start();
    void stop();
    void showScanError(qr::QrError kind, const QString &message);
    [[nodiscard]] auto engine() const -> qr::QrCameraEngine *;
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void backClicked();

public slots:
    void onCameraState(qr::QrCamera::State state);
    void onProgress(int seen, int total);
    void onSettingsClicked();
    void onScanAgainClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    qr::QrCamera *m_camera = nullptr;
    qr::QrCameraEngine *m_engine = nullptr;
    catalog::Label *m_hint = nullptr;
    qr::QrCameraDisplay *m_display = nullptr;
    catalog::ScanProgress *m_progress = nullptr;
    catalog::Notice *m_scan_error = nullptr;
    catalog::Button *m_scan_again = nullptr;
    catalog::Button *m_settings = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

} // namespace keys::flow
