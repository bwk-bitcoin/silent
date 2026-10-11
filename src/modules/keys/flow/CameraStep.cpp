#include "CameraStep.h"

#include "CameraSettings.h"
#include "CameraViewport.h"
#include "QrDisplayStep.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/feedback/ScanProgress.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "theme/Palette.h"

#include <QDebug>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <string>

#include <Qontrol>
#include <common.h>

namespace keys::flow {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {

// The preview contrast scales each channel around mid grey (the design's CSS
// contrast(0.55 + level * 0.28)).
constexpr double CONTRAST_BASE = 0.55;
constexpr double CONTRAST_STEP = 0.28;
constexpr double MID_GREY = 128.0;
constexpr int CHANNEL_LEVELS = 256;
constexpr int MAX_CHANNEL = 255;
constexpr int RGB_CHANNELS = 3;

} // namespace

CameraStep::CameraStep(KeysModule *module, QWidget *parent)
    : QWidget(parent),
      m_module(module),
      m_contrast(QrDisplayStep::DEFAULT_LEVEL) {
    init();
    doConnect();
    view();
    rebuildContrastTable();
}

CameraStep::~CameraStep() {
    stop();
}

void CameraStep::init() {
    m_hint = new Label(LabelRole::Body);
    m_hint->setTone(LabelTone::Secondary);
    m_hint->setWordWrap(true);

    m_viewport = new CameraViewport;
    m_progress = new catalog::ScanProgress;
    m_progress->setVisible(false);

    m_scan_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_scan_again = catalog::btnScanAgain();
    m_scan_error->setAction(m_scan_again);
    m_scan_error->setVisible(false);

    m_settings = catalog::btnCameraSettings();
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::Back);
    m_actions->addAction(m_settings);

    m_preview_timer = new QTimer(this);
    m_preview_timer->setInterval(PREVIEW_INTERVAL_MS);
}

void CameraStep::doConnect() {
    connect(m_preview_timer, &QTimer::timeout, this, &CameraStep::onPreviewTick, qontrol::UNIQUE);
    connect(m_module, &KeysModule::cameraOpened, this, &CameraStep::onCameraOpened,
            qontrol::UNIQUE);
    connect(m_scan_again, &QPushButton::clicked, this, &CameraStep::onScanAgainClicked,
            qontrol::UNIQUE);
    connect(m_settings, &QPushButton::clicked, this, &CameraStep::onSettingsClicked,
            qontrol::UNIQUE);
    connect(m_actions, &catalog::ModalActions::backClicked, this, &CameraStep::backClicked,
            qontrol::UNIQUE);
}

void CameraStep::view() {
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_hint)
        ->push(m_viewport)
        ->push(m_progress)
        ->push(m_scan_error)
        ->into(this);
}

void CameraStep::setHint(const QString &hint) {
    m_hint->setText(hint);
}

void CameraStep::start() {
    m_scan_error->setVisible(false);
    m_progress->setProgress(0, 0);
    m_progress->setVisible(false);
    m_camera_names.clear();
    m_camera_indexes.clear();
    if (m_module->rust() == nullptr) {
        m_viewport->setState(CameraViewport::State::Denied);
        return;
    }
    auto list = m_module->rust()->camera_list();
    if (!list.is_ok) {
        qWarning() << "keys: camera list:" << KeysModule::toQt(list.error);
        m_viewport->setState(CameraViewport::State::Denied);
        return;
    }
    if (list.names.empty()) {
        m_viewport->setState(CameraViewport::State::None);
        return;
    }
    for (std::size_t i = 0; i < list.names.size(); ++i) {
        m_camera_names.append(KeysModule::toQt(list.names[i]));
        m_camera_indexes.append(list.indexes[i]);
    }
    m_camera = std::clamp(m_camera, 0, static_cast<int>(m_camera_indexes.size()) - 1);
    openCamera(m_camera);
}

void CameraStep::stop() {
    m_preview_timer->stop();
    m_req_id = 0;
    m_module->cancelCamera();
}

void CameraStep::openCamera(int camera) {
    m_preview_timer->stop();
    m_camera = camera;
    m_viewport->setState(CameraViewport::State::Starting);
    m_progress->setVisible(false);
    m_module->cancelCamera();
    m_req_id = m_module->nextReqId();
    m_module->openCamera(m_req_id, m_camera_indexes.at(camera));
}

void CameraStep::showScanError(QrErrorKind kind, const QString &message) {
    switch (kind) {
    case QrErrorKind::CameraFailed:
        qWarning() << "keys: camera failed:" << message;
        m_preview_timer->stop();
        m_progress->setVisible(false);
        m_viewport->setState(CameraViewport::State::Failed);
        return;
    case QrErrorKind::RequestIdMismatch:
        m_scan_error->setTitle(TR("flow-scan-wrong-request-title"));
        m_scan_error->setText(TR("flow-scan-wrong-request-body"));
        break;
    case QrErrorKind::SignaturesUnsupported:
        m_scan_error->setTitle(TR("flow-scan-bare-sigs-title"));
        m_scan_error->setText(TR("flow-scan-bare-sigs-body"));
        break;
    case QrErrorKind::Other:
    default:
        m_scan_error->setTitle(TR("flow-scan-other-title"));
        m_scan_error->setText(message);
        break;
    }
    m_scan_error->setVisible(true);
}

auto CameraStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void CameraStep::onCameraOpened(ReqId req_id, bool is_ok, const QString &error) {
    if (req_id != m_req_id) {
        return;
    }
    if (!is_ok) {
        qWarning() << "keys: camera open:" << error;
        m_viewport->setState(CameraViewport::State::Failed);
        return;
    }
    m_last_seq = 0;
    m_viewport->setState(CameraViewport::State::Live);
    m_progress->setVisible(true);
    m_preview_timer->start();
}

void CameraStep::onPreviewTick() {
    if (m_module->rust() == nullptr) {
        return;
    }
    auto frame = m_module->rust()->camera_frame();
    if (frame.is_ok && frame.seq != m_last_seq) {
        m_last_seq = frame.seq;
        m_viewport->setFrame(previewImage(frame));
    }
    auto progress = m_module->rust()->camera_progress();
    if (progress.is_ok) {
        m_progress->setProgress(static_cast<int>(progress.seen), static_cast<int>(progress.total));
    }
}

void CameraStep::onSettingsClicked() {
    auto *settings = new CameraSettings(m_camera_names, m_camera, m_contrast);
    connect(settings, &CameraSettings::cameraPicked, this, &CameraStep::onCameraPicked,
            qontrol::UNIQUE);
    connect(settings, &CameraSettings::contrastPicked, this, &CameraStep::onContrastPicked,
            qontrol::UNIQUE);
    AppController::execModal(settings);
}

void CameraStep::onCameraPicked(int camera) {
    if (camera < 0 || camera >= m_camera_indexes.size()) {
        return;
    }
    openCamera(camera);
}

void CameraStep::onContrastPicked(int contrast) {
    m_contrast = contrast;
    rebuildContrastTable();
}

void CameraStep::onScanAgainClicked() {
    m_scan_error->setVisible(false);
    m_progress->setProgress(0, 0);
    if (m_module->rust() != nullptr) {
        m_module->rust()->camera_reset();
    }
}

void CameraStep::rebuildContrastTable() {
    double factor = CONTRAST_BASE + m_contrast * CONTRAST_STEP;
    m_contrast_table.resize(CHANNEL_LEVELS);
    for (int level = 0; level < CHANNEL_LEVELS; ++level) {
        auto value = static_cast<int>(std::lround((level - MID_GREY) * factor + MID_GREY));
        m_contrast_table[level] = static_cast<std::uint8_t>(std::clamp(value, 0, MAX_CHANNEL));
    }
}

auto CameraStep::previewImage(const CameraFrameResult &frame) const -> QImage {
    auto width = static_cast<int>(frame.width);
    QImage image = QImage(frame.data.data(), width, static_cast<int>(frame.height),
                          width * RGB_CHANNELS, QImage::Format_RGB888)
                       .copy();
    for (int row = 0; row < image.height(); ++row) {
        std::uint8_t *line = image.scanLine(row);
        for (int byte = 0; byte < width * RGB_CHANNELS; ++byte) {
            line[byte] = m_contrast_table.at(line[byte]);
        }
    }
    return image;
}

} // namespace keys::flow
