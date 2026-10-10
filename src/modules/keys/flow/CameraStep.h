#pragma once

#include <cstdint>

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <interfaces/types.h>
#include <silent.h>

class QTimer;

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
class ScanProgress;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::flow {

class CameraViewport;

// Reads an animated QR answer back off a device's screen. Rust owns capture,
// decoding and reassembly; this step picks the camera, paints the preview, counts
// the parts and shows why a scanned code was refused. The decoded answer arrives on
// the module's event channel, not here.
class CameraStep : public QWidget {
    Q_OBJECT

public:
    static constexpr int PREVIEW_INTERVAL_MS = 33;

    explicit CameraStep(KeysModule *module, QWidget *parent = nullptr);
    ~CameraStep() override;
    void setHint(const QString &hint);
    // Opens the camera picked last, from a clean scan.
    void start();
    void stop();
    void showScanError(QrErrorKind kind, const QString &message);
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void backClicked();

public slots:
    void onPreviewTick();
    void onCameraOpened(ReqId req_id, bool is_ok, const QString &error);
    void onSettingsClicked();
    void onCameraPicked(int camera);
    void onContrastPicked(int contrast);
    void onScanAgainClicked();

protected:
    void init();
    void doConnect();
    void view();
    void openCamera(int camera);
    void rebuildContrastTable();
    [[nodiscard]] auto previewImage(const CameraFrameResult &frame) const -> QImage;

private:
    // Non-owning: the keys module outlives every flow it opens.
    KeysModule *m_module = nullptr;
    catalog::Label *m_hint = nullptr;
    CameraViewport *m_viewport = nullptr;
    catalog::ScanProgress *m_progress = nullptr;
    catalog::Notice *m_scan_error = nullptr;
    catalog::Button *m_scan_again = nullptr;
    catalog::Button *m_settings = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    QTimer *m_preview_timer = nullptr;
    QStringList m_camera_names;
    QList<std::uint32_t> m_camera_indexes;
    int m_camera = 0;
    int m_contrast;
    // Per channel value after the preview contrast, rebuilt only when it changes.
    QList<std::uint8_t> m_contrast_table;
    // The camera request openCamera() made last; an older answer is ignored.
    ReqId m_req_id = 0;
    std::uint64_t m_last_seq = 0;
};

} // namespace keys::flow
