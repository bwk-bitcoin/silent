#include "QrRoute.h"

#include "modules/keys/KeysModule.h"
#include "modules/keys/modals/CameraScan.h"
#include "modules/keys/modals/QrDisplay.h"
#include "modules/keys/signers/RouteSigner.h"

#include "AppController.h"
#include "i18n/Tr.h"

#include <QDialog>
#include <QThread>

#include <cstdint>
#include <utility>

#include <common.h>

namespace keys {

namespace {

// Density maps to the payload each frame carries: MIN_LEVEL is the smallest, most
// scannable frames; each step up packs more bytes (and fewer frames) per QR code.
constexpr std::uint32_t MIN_PART_BYTES = 50;
constexpr std::uint32_t PART_BYTES_STEP = 50;

auto densityToPartBytes(int density) -> std::uint32_t {
    return MIN_PART_BYTES +
           static_cast<std::uint32_t>(density - modal::QrDisplay::MIN_LEVEL) * PART_BYTES_STEP;
}

// Translates the two QR response validation failures that get their own message
// (qr::Error::RequestIdMismatch, qr::Error::SignaturesUnsupported); Rust tags them
// via KeyEvent::error_kind so this switches on a stable discriminant rather than
// matching the Display text. Any other camera-sourced error passes through as-is.
auto cameraQrErrorMessage(const KeyEvent &event) -> QString {
    switch (event.error_kind) {
    case QrErrorKind::RequestIdMismatch:
        return TR("keys-qr-response-mismatch");
    case QrErrorKind::SignaturesUnsupported:
        return TR("keys-qr-response-partial");
    case QrErrorKind::Other:
    default:
        return KeysModule::toQt(event.error);
    }
}

} // namespace

QrRoute::QrRoute(KeysModule *module)
    : QObject(module),
      m_module(module),
      m_density(modal::QrDisplay::DEFAULT_LEVEL) {
    connect(this, &QrRoute::qrRequestReady, this, &QrRoute::onQrRequestReady, qontrol::UNIQUE);
}

void QrRoute::startSign(ReqId req_id, RouteSigner *signer, QString psbt_v2_hex,
                        QString descriptor) {
    if (m_flow.has_value()) {
        m_module->untrackPendingRequest(req_id);
        if (signer != nullptr) {
            signer->deliverError(req_id, TR("keys-qr-busy"));
        }
        return;
    }
    m_flow = QrFlow{.kind = QrFlow::Kind::Sign,
                    .req_id = req_id,
                    .signer = signer,
                    .pending_id = 0,
                    .psbt_hex = std::move(psbt_v2_hex),
                    .descriptor = std::move(descriptor),
                    .verify_path = QString(),
                    .verify_address = QString()};
    buildSignRequest();
}

void QrRoute::startVerify(ReqId req_id, RouteSigner *signer, QString descriptor, QString path,
                          QString address) {
    if (m_flow.has_value()) {
        m_module->untrackPendingRequest(req_id);
        if (signer != nullptr) {
            signer->deliverError(req_id, TR("keys-qr-busy"));
        }
        return;
    }
    m_flow = QrFlow{.kind = QrFlow::Kind::Verify,
                    .req_id = req_id,
                    .signer = signer,
                    .pending_id = 0,
                    .psbt_hex = QString(),
                    .descriptor = std::move(descriptor),
                    .verify_path = std::move(path),
                    .verify_address = std::move(address)};
    buildVerifyRequest();
}

void QrRoute::startSpDescriptor(ReqId req_id) {
    if (m_flow.has_value()) {
        m_module->untrackPendingRequest(req_id);
        m_module->deliverSpDescriptorError(req_id, TR("keys-qr-busy"));
        return;
    }
    m_flow = QrFlow{.kind = QrFlow::Kind::SpDescriptor,
                    .req_id = req_id,
                    .signer = nullptr,
                    .pending_id = 0,
                    .psbt_hex = QString(),
                    .descriptor = QString(),
                    .verify_path = QString(),
                    .verify_address = QString()};
    buildSpDescriptorRequest();
}

void QrRoute::buildSignRequest() {
    auto *r = m_module->rust();
    if (r == nullptr) {
        finishFlow(Outcome::Error, TR("keys-route-unavailable"));
        return;
    }

    auto reserved = r->qr_reserve_sign();
    if (!reserved.is_ok) {
        finishFlow(Outcome::Error, KeysModule::toQt(reserved.error));
        return;
    }
    m_flow->pending_id = reserved.id;

    rust::Vec<rust::String> descriptors;
    descriptors.push_back(rust::String(m_flow->descriptor.toStdString()));
    auto reqId = reserved.id;
    auto psbtHex = m_flow->psbt_hex.toStdString();
    auto partBytes = densityToPartBytes(m_density);
    auto *thread =
        QThread::create([this, reqId, descriptors = std::move(descriptors), psbtHex, partBytes]() {
            auto request = ::qr_build_sign(reqId, descriptors, psbtHex, partBytes);
            emit qrRequestReady(request.into_raw());
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void QrRoute::buildVerifyRequest() {
    auto *r = m_module->rust();
    if (r == nullptr) {
        finishFlow(Outcome::Error, TR("keys-route-unavailable"));
        return;
    }
    auto request = r->qr_request_verify_address(rust::String(m_flow->descriptor.toStdString()),
                                                rust::String(m_flow->verify_path.toStdString()),
                                                rust::String(m_flow->verify_address.toStdString()),
                                                densityToPartBytes(m_density));
    emit qrRequestReady(request.into_raw());
}

void QrRoute::buildSpDescriptorRequest() {
    auto *r = m_module->rust();
    if (r == nullptr) {
        finishFlow(Outcome::Error, TR("keys-route-unavailable"));
        return;
    }
    auto request = r->qr_request_get_sp_descriptor(densityToPartBytes(m_density));
    emit qrRequestReady(request.into_raw());
}

void QrRoute::onQrRequestReady(QrRequest *request) {
    auto box = rust::Box<QrRequest>::from_raw(request);
    if (!m_flow.has_value()) {
        // The flow was already answered by the time this request finished
        // building: dropping `box` here discards it rather than reopening a modal
        // for a flow that has ended.
        return;
    }
    if (m_flow->kind == QrFlow::Kind::Sign) {
        if (box->req_id() != m_flow->pending_id) {
            // A newer density rebuild reserved a later id; this build is stale.
            return;
        }
        m_flow->pending_id = 0;
        auto *r = m_module->rust();
        if (r == nullptr) {
            finishFlow(Outcome::Error, TR("keys-route-unavailable"));
            return;
        }
        auto result = r->qr_commit_request(*box);
        if (!result.is_ok) {
            finishFlow(Outcome::Error, KeysModule::toQt(result.error));
            return;
        }
    }
    showRequest(std::move(box));
}

void QrRoute::showRequest(rust::Box<QrRequest> request) {
    if (m_display.isNull()) {
        auto *display = new modal::QrDisplay(TR("keys-qr-title"), std::move(request));
        m_display = display;
        connect(display, &modal::QrDisplay::densityChanged, this, &QrRoute::onDensityChanged,
                qontrol::UNIQUE);
        connect(display, &modal::QrDisplay::nextRequested, this, &QrRoute::onDisplayNext,
                qontrol::UNIQUE);
        connect(display, &QDialog::rejected, this, &QrRoute::onDisplayRejected, qontrol::UNIQUE);
        AppController::execModal(display);
        return;
    }
    m_display->setRequest(std::move(request));
}

void QrRoute::onDensityChanged(int density) {
    m_density = density;
    if (!m_flow.has_value()) {
        return;
    }
    switch (m_flow->kind) {
    case QrFlow::Kind::Sign:
        buildSignRequest();
        return;
    case QrFlow::Kind::Verify:
        buildVerifyRequest();
        return;
    case QrFlow::Kind::SpDescriptor:
        buildSpDescriptorRequest();
        return;
    }
}

void QrRoute::onDisplayNext() {
    if (!m_flow.has_value()) {
        return;
    }
    if (m_module->rust() == nullptr) {
        finishFlow(Outcome::Error, TR("keys-route-unavailable"));
        return;
    }

    auto *scan = new modal::CameraScan(TR("keys-camera-title"), m_module);
    m_camera_scan = scan;
    connect(scan, &modal::CameraScan::previousRequested, this, &QrRoute::onCameraScanPrevious,
            qontrol::UNIQUE);
    connect(scan, &modal::CameraScan::cancelled, this, &QrRoute::onCameraScanCancelled,
            qontrol::UNIQUE);
    AppController::execModal(scan);
}

void QrRoute::onCameraScanPrevious() {
    if (!m_display.isNull()) {
        m_display->restartAnimation();
    }
}

void QrRoute::onCameraScanCancelled() {
    finishFlow(Outcome::Error, TR("keys-signing-cancelled"));
}

void QrRoute::onDisplayRejected() {
    finishFlow(Outcome::Error, TR("keys-signing-cancelled"));
}

void QrRoute::onKeyEvent(const KeyEvent &event) {
    switch (event.flag) {
    case KeyEventFlag::Error:
        // Camera-sourced errors (a mismatched or unsupported response, a device
        // that disappeared mid-scan) carry no req_id: the outstanding camera scan,
        // if one is open, is the only thing they can be answering. event.req_id == 0
        // is what actually marks an Error as camera-unsolicited; without that check,
        // an unrelated Error with a genuine req_id arriving while the scan is open
        // would be misrouted here instead of to the bus.
        if (!m_camera_scan.isNull() && m_flow.has_value() && event.req_id == 0) {
            finishFlow(Outcome::Error, cameraQrErrorMessage(event));
            return;
        }
        if (event.req_id == 0) {
            qWarning() << "keys:" << KeysModule::toQt(event.error);
            return;
        }
        m_module->bus()->deliverError(event.req_id, KeysModule::toQt(event.error));
        return;
    case KeyEventFlag::QrDecoded:
        // `event.payload` already went through the session's response validation
        // (request id match, expected message type, no bare-signatures response) on
        // the Rust side before this event was emitted; a failed check surfaces as
        // KeyEventFlag::Error above instead. Here it is always the matching
        // outstanding flow's answer: a signed PSBT v2 hex for a sign flow, a
        // match/mismatch bool (event.ok) for a verify flow, or an sp() descriptor
        // for a spDescriptor flow, ready to deliver as-is.
        if (m_camera_scan.isNull() || !m_flow.has_value()) {
            return;
        }
        switch (m_flow->kind) {
        case QrFlow::Kind::Sign:
            finishFlow(Outcome::Signed, KeysModule::toQt(event.payload));
            return;
        case QrFlow::Kind::Verify:
            finishFlow(event.ok);
            return;
        case QrFlow::Kind::SpDescriptor:
            finishFlow(Outcome::Descriptor, KeysModule::toQt(event.payload));
            return;
        }
        return;
    case KeyEventFlag::ScanProgress:
        if (!m_camera_scan.isNull() && m_module->rust() != nullptr) {
            auto progress = m_module->rust()->camera_progress();
            if (progress.is_ok) {
                m_camera_scan->setProgress(progress.seen, progress.total);
            }
        }
        return;
    case KeyEventFlag::CameraStopped:
        // Every capture-thread exit, including the one this route triggers itself
        // via camera_stop(), reports this. A failure already arrived as its own
        // Error event just before it, so there is nothing left to do here.
        return;
    default:
        return;
    }
}

void QrRoute::finishFlow(bool matched) {
    if (!m_flow.has_value()) {
        return;
    }
    QrFlow flow = std::move(*m_flow);
    m_flow.reset();
    if (!flow.signer.isNull()) {
        flow.signer->deliverDisplayed(flow.req_id, matched);
    }
    closeOutstanding(flow.req_id);
}

void QrRoute::finishFlow(Outcome outcome, const QString &text) {
    if (!m_flow.has_value()) {
        return;
    }
    QrFlow flow = std::move(*m_flow);
    m_flow.reset();
    switch (outcome) {
    case Outcome::Signed:
        if (!flow.signer.isNull()) {
            flow.signer->deliverSigned(flow.req_id, text);
        }
        break;
    case Outcome::Descriptor:
        m_module->validateAndDeliverSpDescriptor(flow.req_id, text);
        break;
    case Outcome::Error:
        if (flow.kind == QrFlow::Kind::SpDescriptor) {
            m_module->deliverSpDescriptorError(flow.req_id, text);
        } else if (!flow.signer.isNull()) {
            flow.signer->deliverError(flow.req_id, text);
        }
        break;
    }
    closeOutstanding(flow.req_id);
}

void QrRoute::closeOutstanding(ReqId req_id) {
    m_module->untrackPendingRequest(req_id);
    if (!m_camera_scan.isNull() && m_camera_scan->isVisible()) {
        m_camera_scan->reject();
    }
    if (!m_display.isNull() && m_display->isVisible()) {
        m_display->reject();
    }
}

} // namespace keys
