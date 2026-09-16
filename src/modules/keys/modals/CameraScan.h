#pragma once

#include <cstdint>

#include <QString>

#include <Qontrol>
#include <interfaces/types.h>

class QLabel;
class QTimer;

namespace catalog {
class Button;
class ComboBox;
class Label;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

// Reads an animated QR response back off an airgapped device's screen. Rust owns
// capture, downscaling, QR detection, BBQR reassembly and protocol decoding; this
// modal only lets the user pick a camera, paints the latest preview frame, and
// shows how many parts have been seen. The decoded response arrives on the
// module's KeyEvent channel, not through a return value from this class.
//
// The camera worker thread is owned by KeysModule, not this modal: it outlives a
// modal closed (Escape, window close) while a camera is still opening, so a late
// answer is dropped instead of landing on a freed object.
class CameraScan final : public qontrol::Modal {
    Q_OBJECT

public:
    static constexpr int PREVIEW_INTERVAL_MS = 33;

    CameraScan(const QString &title, KeysModule *module, QWidget *parent = nullptr);
    ~CameraScan() override;

    void setProgress(std::uint32_t parts_seen, std::uint32_t parts_total);

signals:
    void previousRequested();
    void cancelled();

public slots:
    void onPreviewTick();
    void onCameraChanged();
    void onCameraOpened(ReqId req_id, bool is_ok, QString error);
    void onPreviousClicked();
    void onCancelClicked();

protected:
    void init();
    void doConnect();
    void view();
    void startCamera(std::int32_t camera_index);
    void stopCamera();
    void populateCameras();
    void showUnavailable(const QString &reason, bool disable_combo = true);
    // Shared by onPreviewTick's own poll and setProgress's externally pushed
    // values, so both format the label the same way.
    void updateProgressLabel(std::uint32_t seen, std::uint32_t total);

private:
    // Non-owning: the keys module outlives every modal it opens.
    KeysModule *m_module = nullptr;
    // The req_id startCamera() last asked openCamera() for, so onCameraOpened()
    // can ignore an answer meant for a camera switch this modal already moved on
    // from.
    ReqId m_req_id = 0;
    QTimer *m_preview_timer = nullptr;
    QLabel *m_preview = nullptr;
    catalog::Label *m_placeholder = nullptr;
    catalog::Label *m_progress_label = nullptr;
    catalog::ComboBox *m_camera_combo = nullptr;
    catalog::Button *m_previous_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    std::uint64_t m_last_seq = 0;
    bool m_camera_running = false;
};

} // namespace keys::modal
