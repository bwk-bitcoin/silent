#include "BwkQrEngine.h"

#include "KeysModule.h"
#include "KeysView.h"

#include "i18n/Tr.h"

#include <QDebug>

#include <cstdint>

namespace keys {

namespace {

auto toQrError(QrErrorKind kind) -> qr::QrError {
    switch (kind) {
    case QrErrorKind::RequestIdMismatch:
        return qr::QrError::WrongRequest;
    case QrErrorKind::SignaturesUnsupported:
        return qr::QrError::Unsupported;
    case QrErrorKind::Other:
    default:
        return qr::QrError::Refused;
    }
}

} // namespace

BwkQrEngine::BwkQrEngine(KeysModule *module, QObject *parent)
    : qr::QrEngine(parent),
      m_module(module) {
}

void BwkQrEngine::getXpubs(const QStringList &paths) {
    if (m_module->rust() == nullptr) {
        emit error(qr::QrError::RequestFailed, TR("keys-module-not-ready"));
        return;
    }
    rust::Vec<rust::String> rustPaths;
    for (const auto &path : paths) {
        rustPaths.push_back(rust::String(path.toStdString()));
    }
    issue(m_module->rust()->qr_request_get_xpubs(std::move(rustPaths)));
}

void BwkQrEngine::getSpDescriptor() {
    if (m_module->rust() == nullptr) {
        emit error(qr::QrError::RequestFailed, TR("keys-module-not-ready"));
        return;
    }
    issue(m_module->rust()->qr_request_get_sp_descriptor());
}

void BwkQrEngine::sign(const QString &psbt_v2_hex, const QString &descriptor) {
    if (m_module->rust() == nullptr) {
        emit error(qr::QrError::RequestFailed, TR("keys-module-not-ready"));
        return;
    }
    rust::Vec<rust::String> descriptors;
    descriptors.push_back(rust::String(descriptor.toStdString()));
    issue(m_module->rust()->qr_request_sign(std::move(descriptors),
                                            rust::Str(psbt_v2_hex.toStdString())));
}

void BwkQrEngine::verifyAddress(const QString &descriptor, const QString &path,
                                const QString &address) {
    if (m_module->rust() == nullptr) {
        emit error(qr::QrError::RequestFailed, TR("keys-module-not-ready"));
        return;
    }
    issue(m_module->rust()->qr_request_verify_address(rust::Str(descriptor.toStdString()),
                                                      rust::Str(path.toStdString()),
                                                      rust::Str(address.toStdString())));
}

void BwkQrEngine::stop() {
    m_waiting = false;
}

void BwkQrEngine::issue(const QrMessageResult &message) {
    if (!message.is_ok) {
        m_waiting = false;
        emit error(qr::QrError::RequestFailed, KeysModule::toQt(message.error));
        return;
    }
    m_waiting = true;
    emit request(
        qr::QrRequest{.payload = QByteArray(reinterpret_cast<const char *>(message.payload.data()),
                                            static_cast<qsizetype>(message.payload.size()))});
}

void BwkQrEngine::response(const qr::QrResponse &response) {
    if (!m_waiting || m_module->rust() == nullptr) {
        return;
    }
    auto decoded = m_module->rust()->qr_decode_response(rust::Slice<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(response.payload.data()), response.payload.size()));
    if (!decoded->is_ok()) {
        // A refused answer leaves the request outstanding: the scan goes on.
        emit error(toQrError(decoded->error_kind()), KeysModule::toQt(decoded->get_error()));
        return;
    }
    m_waiting = false;
    forwardResponse(*decoded);
}

void BwkQrEngine::forwardResponse(const ::QrResponse &decoded) {
    switch (decoded.kind()) {
    case QrResponseKind::Xpubs: {
        auto entries = decoded.xpubs();
        if (entries.empty()) {
            emit error(qr::QrError::RequestFailed, TR("keys-qr-wrong-kind"));
            return;
        }
        QList<qr::QrXpub> xpubList;
        for (const auto &entry : entries) {
            xpubList.append({.path = xpubPath(KeysModule::toQt(entry.path)),
                             .xpub = KeysModule::toQt(entry.xpub)});
        }
        qr::QrDevice device{.vendor = KeysModule::toQt(decoded.vendor()),
                            .model = KeysModule::toQt(decoded.model()),
                            .version = KeysModule::toQt(decoded.version()),
                            .capabilities = decoded.capabilities()};
        emit xpubs(KeysModule::toQt(decoded.fingerprint()), xpubList, device);
        return;
    }
    case QrResponseKind::SpDescriptor:
        emit spDescriptor(KeysModule::toQt(decoded.descriptor()));
        return;
    case QrResponseKind::SignedPsbt:
        emit signedPsbt(KeysModule::toQt(decoded.psbt_hex()));
        return;
    case QrResponseKind::AddressVerified:
        emit addressVerified(decoded.verified());
        return;
    case QrResponseKind::Registration:
    default:
        // Nothing asks this codec for a registration.
        qWarning() << "keys: unexpected QR registration answer";
        return;
    }
}

} // namespace keys
