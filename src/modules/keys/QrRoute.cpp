#include "QrRoute.h"

#include "modules/keys/KeysModule.h"
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
    m_flow = QrFlow{.req_id = req_id,
                    .signer = signer,
                    .pending_id = 0,
                    .psbt_hex = std::move(psbt_v2_hex),
                    .descriptor = std::move(descriptor)};
    buildSignRequest();
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

void QrRoute::onQrRequestReady(QrRequest *request) {
    auto box = rust::Box<QrRequest>::from_raw(request);
    if (!m_flow.has_value()) {
        // The flow was already answered by the time this request finished
        // building: dropping `box` here discards it rather than reopening a modal
        // for a flow that has ended.
        return;
    }
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
    buildSignRequest();
}

void QrRoute::onDisplayNext() {
    finishFlow(Outcome::Error, TR("keys-scan-unavailable"));
}

void QrRoute::onDisplayRejected() {
    finishFlow(Outcome::Error, TR("keys-signing-cancelled"));
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
    case Outcome::Error:
        if (!flow.signer.isNull()) {
            flow.signer->deliverError(flow.req_id, text);
        }
        break;
    }
    closeOutstanding(flow.req_id);
}

void QrRoute::closeOutstanding(ReqId req_id) {
    m_module->untrackPendingRequest(req_id);
    if (!m_display.isNull() && m_display->isVisible()) {
        m_display->reject();
    }
}

} // namespace keys
