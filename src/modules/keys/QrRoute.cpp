#include "QrRoute.h"

#include "modules/keys/KeysModule.h"
#include "modules/keys/flow/QrDisplayStep.h"

#include "i18n/Tr.h"

#include <QDebug>
#include <QThread>

#include <cstdint>
#include <utility>

#include <common.h>

namespace keys {

namespace {

// Density maps to the payload each frame carries: the lowest level packs the least,
// most scannable frames; each step up packs more bytes and fewer frames.
constexpr std::uint32_t MIN_PART_BYTES = 50;
constexpr std::uint32_t PART_BYTES_STEP = 50;

} // namespace

QrRoute::QrRoute(KeysModule *module) : QObject(module), m_module(module) {
    connect(this, &QrRoute::requestBuilt, this, &QrRoute::onRequestBuilt, qontrol::UNIQUE);
}

void QrRoute::startSign(flow::QrDisplayStep *display, QString psbt_v2_hex, QString descriptor) {
    start(display, Exchange{.kind = Kind::Sign,
                            .pending_id = 0,
                            .psbt_hex = std::move(psbt_v2_hex),
                            .descriptor = std::move(descriptor),
                            .path = QString(),
                            .address = QString()});
}

void QrRoute::startVerify(flow::QrDisplayStep *display, QString descriptor, QString path,
                          QString address) {
    start(display, Exchange{.kind = Kind::Verify,
                            .pending_id = 0,
                            .psbt_hex = QString(),
                            .descriptor = std::move(descriptor),
                            .path = std::move(path),
                            .address = std::move(address)});
}

void QrRoute::startSpDescriptor(flow::QrDisplayStep *display) {
    start(display, Exchange{.kind = Kind::SpDescriptor});
}

void QrRoute::start(flow::QrDisplayStep *display, Exchange exchange) {
    stop();
    m_exchange = std::move(exchange);
    m_display = display;
    connect(display, &flow::QrDisplayStep::densityChanged, this, &QrRoute::onDensityChanged,
            qontrol::UNIQUE);
    display->setRebuilding();
    build();
}

void QrRoute::stop() {
    m_exchange.reset();
    if (!m_display.isNull()) {
        disconnect(m_display, &flow::QrDisplayStep::densityChanged, this,
                   &QrRoute::onDensityChanged);
    }
    m_display = nullptr;
}

void QrRoute::onDensityChanged([[maybe_unused]] int density) {
    if (m_exchange.has_value()) {
        build();
    }
}

void QrRoute::build() {
    if (m_module->rust() == nullptr) {
        fail(TR("keys-module-not-ready"));
        return;
    }
    switch (m_exchange->kind) {
    case Kind::Sign:
        buildSign();
        return;
    case Kind::Verify:
        buildVerify();
        return;
    case Kind::SpDescriptor:
        buildSpDescriptor();
        return;
    }
}

void QrRoute::buildSign() {
    auto reserved = m_module->rust()->qr_reserve_sign();
    if (!reserved.is_ok) {
        fail(KeysModule::toQt(reserved.error));
        return;
    }
    m_exchange->pending_id = reserved.id;

    rust::Vec<rust::String> descriptors;
    descriptors.push_back(rust::String(m_exchange->descriptor.toStdString()));
    auto reqId = reserved.id;
    auto psbtHex = m_exchange->psbt_hex.toStdString();
    auto bytes = partBytes();
    auto *thread =
        QThread::create([this, reqId, descriptors = std::move(descriptors), psbtHex, bytes]() {
            auto request = ::qr_build_sign(reqId, descriptors, psbtHex, bytes);
            emit requestBuilt(request.into_raw());
        });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void QrRoute::buildVerify() {
    show(m_module->rust()->qr_request_verify_address(
        rust::String(m_exchange->descriptor.toStdString()),
        rust::String(m_exchange->path.toStdString()),
        rust::String(m_exchange->address.toStdString()), partBytes()));
}

void QrRoute::buildSpDescriptor() {
    show(m_module->rust()->qr_request_get_sp_descriptor(partBytes()));
}

void QrRoute::onRequestBuilt(QrRequest *request) {
    auto box = rust::Box<QrRequest>::from_raw(request);
    if (!m_exchange.has_value() || box->req_id() != m_exchange->pending_id) {
        // The exchange ended, or a newer density rebuild reserved a later id:
        // dropping `box` discards this build.
        return;
    }
    m_exchange->pending_id = 0;
    if (m_module->rust() == nullptr) {
        fail(TR("keys-module-not-ready"));
        return;
    }
    auto committed = m_module->rust()->qr_commit_request(*box);
    if (!committed.is_ok) {
        fail(KeysModule::toQt(committed.error));
        return;
    }
    show(std::move(box));
}

void QrRoute::show(rust::Box<QrRequest> request) {
    if (!request->is_ok()) {
        fail(KeysModule::toQt(request->get_error()));
        return;
    }
    if (!m_display.isNull()) {
        m_display->setRequest(std::move(request));
    }
}

void QrRoute::fail(const QString &message) {
    stop();
    emit failed(message);
}

auto QrRoute::partBytes() const -> std::uint32_t {
    int density = m_display.isNull() ? flow::QrDisplayStep::DEFAULT_LEVEL : m_display->density();
    return MIN_PART_BYTES +
           static_cast<std::uint32_t>(density - flow::QrDisplayStep::MIN_LEVEL) * PART_BYTES_STEP;
}

void QrRoute::onKeyEvent(const KeyEvent &event) {
    switch (event.flag) {
    case KeyEventFlag::Error:
        // A camera-sourced error carries no req_id: it can only concern the scan in
        // progress. One with a req_id answers a request made through the bus.
        if (event.req_id != 0) {
            m_module->bus()->deliverError(event.req_id, KeysModule::toQt(event.error));
            return;
        }
        if (!m_exchange.has_value()) {
            qWarning() << "keys:" << KeysModule::toQt(event.error);
            return;
        }
        emit scanFailed(event.error_kind, KeysModule::toQt(event.error));
        return;
    case KeyEventFlag::QrDecoded: {
        // The payload already passed the session's response checks on the Rust side
        // (request id, expected message type); a failed check arrives as an Error.
        if (!m_exchange.has_value()) {
            return;
        }
        Kind kind = m_exchange->kind;
        stop();
        switch (kind) {
        case Kind::Sign:
            emit psbtSigned(KeysModule::toQt(event.payload));
            return;
        case Kind::Verify:
            emit addressVerified(event.ok);
            return;
        case Kind::SpDescriptor:
            emit descriptorDecoded(KeysModule::toQt(event.payload));
            return;
        }
        return;
    }
    case KeyEventFlag::ScanProgress:
    case KeyEventFlag::CameraStopped:
    default:
        // The camera step polls the progress itself, and a stop that failed already
        // came as its own Error.
        return;
    }
}

} // namespace keys
