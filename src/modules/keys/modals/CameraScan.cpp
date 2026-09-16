#include "CameraScan.h"

#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/ComboBox.h"
#include "common.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QTimer>

#include <cstdint>
#include <string>

namespace keys::modal {

using catalog::Button;
using catalog::ComboBox;
using catalog::Label;
using catalog::LabelRole;

namespace {

// A 4:3 box, matching the design's preview area, big enough to read a device
// screen's animated QR code clearly.
constexpr int PREVIEW_WIDTH = 480;
constexpr int PREVIEW_HEIGHT = 360;

} // namespace

CameraScan::CameraScan(const QString &title, KeysModule *module, QWidget *parent)
    : m_module(module) {
    Q_UNUSED(parent);
    setWindowTitle(title);
    init();
    doConnect();
    view();
    populateCameras();
}

CameraScan::~CameraScan() {
    stopCamera();
}

void CameraScan::init() {
    m_preview = new QLabel;
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    m_preview->setVisible(false);

    m_placeholder = new Label(QString(), LabelRole::Body);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setMinimumSize(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    m_placeholder->setVisible(false);

    m_progress_label = new Label(TR("keys-camera-waiting"), LabelRole::Mono);

    m_camera_combo = new ComboBox;

    m_previous_btn = new Button(TR("keys-camera-previous"));
    m_previous_btn->setIcon(icon::chevronLeft());

    m_cancel_btn = new Button(TR("common-cancel"));

    m_preview_timer = new QTimer(this);
    m_preview_timer->setInterval(PREVIEW_INTERVAL_MS);
}

void CameraScan::doConnect() {
    connect(m_preview_timer, &QTimer::timeout, this, &CameraScan::onPreviewTick, qontrol::UNIQUE);
    connect(m_camera_combo, &QComboBox::currentIndexChanged, this, &CameraScan::onCameraChanged,
            qontrol::UNIQUE);
    connect(m_module, &KeysModule::cameraOpened, this, &CameraScan::onCameraOpened,
            qontrol::UNIQUE);
    connect(m_previous_btn, &QPushButton::clicked, this, &CameraScan::onPreviousClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &CameraScan::onCancelClicked,
            qontrol::UNIQUE);
}

void CameraScan::view() {
    auto *previewCol = (new qontrol::Column)->push(m_preview)->push(m_placeholder);

    auto *sourceCol = (new qontrol::Column)
                          ->push(new Label(TR("keys-camera-source"), LabelRole::Caption))
                          ->push(m_camera_combo);

    auto *controlsRow = (new qontrol::Row)
                            ->push(m_previous_btn)
                            ->pushSpacer()
                            ->push(sourceCol)
                            ->pushSpacer()
                            ->push(m_cancel_btn);

    auto *progressRow = (new qontrol::Row)->pushSpacer()->push(m_progress_label)->pushSpacer();

    auto *col = (new qontrol::Column)
                    ->push(frame(previewCol))
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(progressRow)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(controlsRow);

    setMainWidget(margin(col));
}

void CameraScan::populateCameras() {
    auto list = m_module->rust()->camera_list();
    if (!list.is_ok) {
        showUnavailable(TR("keys-camera-unsupported") + QStringLiteral("\n") +
                        QString::fromStdString(std::string(list.error.c_str())));
        return;
    }
    if (list.names.empty()) {
        showUnavailable(TR("keys-camera-none"));
        return;
    }
    // QComboBox::addItem on the first entry fires currentIndexChanged as a side
    // effect; block it here so population can't accidentally start the camera
    // before the list is complete, and start it explicitly below instead.
    m_camera_combo->blockSignals(true);
    for (std::size_t i = 0; i < list.names.size(); ++i) {
        m_camera_combo->addItem(QString::fromStdString(std::string(list.names[i].c_str())),
                                static_cast<int>(list.indexes[i]));
    }
    m_camera_combo->blockSignals(false);
    startCamera(m_camera_combo->currentData().toInt());
}

void CameraScan::startCamera(std::int32_t camera_index) {
    m_module->cancelCamera();
    m_req_id = m_module->nextReqId();
    m_module->openCamera(m_req_id, static_cast<std::uint32_t>(camera_index));
}

void CameraScan::stopCamera() {
    m_preview_timer->stop();
    m_module->cancelCamera();
    m_camera_running = false;
}

void CameraScan::showUnavailable(const QString &reason, bool disable_combo) {
    m_preview_timer->stop();
    m_camera_running = false;
    m_preview->setVisible(false);
    m_placeholder->setText(reason);
    m_placeholder->setVisible(true);
    m_previous_btn->setEnabled(false);
    if (disable_combo) {
        m_camera_combo->setEnabled(false);
    }
}

void CameraScan::updateProgressLabel(std::uint32_t seen, std::uint32_t total) {
    if (total == 0) {
        m_progress_label->setText(TR("keys-camera-waiting"));
        return;
    }
    m_progress_label->setText(TR("keys-camera-progress").arg(seen).arg(total));
}

void CameraScan::onPreviewTick() {
    auto frame = m_module->rust()->camera_frame();
    if (frame.is_ok && frame.seq != m_last_seq) {
        m_last_seq = frame.seq;
        auto image =
            QImage(frame.data.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                   static_cast<int>(frame.width) * 3, QImage::Format_RGB888);
        m_preview->setPixmap(
            QPixmap::fromImage(image.copy())
                .scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    auto progress = m_module->rust()->camera_progress();
    if (progress.is_ok) {
        updateProgressLabel(progress.seen, progress.total);
    }
}

void CameraScan::onCameraChanged() {
    startCamera(m_camera_combo->currentData().toInt());
}

void CameraScan::onCameraOpened(ReqId req_id, bool is_ok, QString error) {
    if (req_id != m_req_id) {
        return;
    }
    if (!is_ok) {
        // The camera list was already populated successfully to reach this point, so
        // this is a single entry failing to start, not the list being unavailable:
        // leave the combo enabled so the user can pick a different camera.
        showUnavailable(error, false);
        return;
    }
    m_camera_running = true;
    m_last_seq = 0;
    m_camera_combo->setEnabled(true);
    m_previous_btn->setEnabled(true);
    m_placeholder->setVisible(false);
    m_preview->setVisible(true);
    m_preview_timer->start();
}

void CameraScan::onPreviousClicked() {
    stopCamera();
    emit previousRequested();
    reject();
}

void CameraScan::onCancelClicked() {
    stopCamera();
    emit cancelled();
    reject();
}

void CameraScan::setProgress(std::uint32_t parts_seen, std::uint32_t parts_total) {
    updateProgressLabel(parts_seen, parts_total);
}

} // namespace keys::modal
