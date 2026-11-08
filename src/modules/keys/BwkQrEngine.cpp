#include "BwkQrEngine.h"

#include "KeysModule.h"
#include "KeysView.h"

#include "i18n/Tr.h"

#include <QDebug>
#include <QThread>

#include <common.h>
#include <cstdint>
#include <utility>

namespace keys {

namespace {

auto toQrError(QrErrorKind kind) -> qr::QrError {
    switch (kind) {
    case QrErrorKind::RequestIdMismatch:
        return qr::QrError::WrongRequest;
    case QrErrorKind::SignaturesUnsupported:
        return qr::QrError::Unsupported;
    case QrErrorKind::MusigSession:
        return qr::QrError::MusigSession;
    case QrErrorKind::Other:
    default:
        return qr::QrError::Refused;
    }
}

auto toQrXpubs(const rust::Vec<KeyXpub> &entries) -> QList<qr::QrXpub> {
    QList<qr::QrXpub> xpubs;
    for (const auto &entry : entries) {
        xpubs.append(
            {.path = xpubPath(KeysModule::toQt(entry.path)), .xpub = KeysModule::toQt(entry.xpub)});
    }
    return xpubs;
}

} // namespace

BwkQrEngine::BwkQrEngine(KeysModule *module, QObject *parent)
    : qr::QrEngine(parent),
      m_module(module) {
    // The workers answer through queued signals: the boxes are reclaimed and the
    // request issued only if these stay connected.
    connect(this, &BwkQrEngine::musigPrepareReady, this, &BwkQrEngine::onMusigPrepareReady,
            qontrol::UNIQUE);
    connect(this, &BwkQrEngine::musigSignReady, this, &BwkQrEngine::onMusigSignReady,
            qontrol::UNIQUE);
    connect(this, &BwkQrEngine::musigAccepted, this, &BwkQrEngine::onMusigAccepted,
            qontrol::UNIQUE);
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

void BwkQrEngine::getSpAccount() {
    if (m_module->rust() == nullptr) {
        emit error(qr::QrError::RequestFailed, TR("keys-module-not-ready"));
        return;
    }
    issue(m_module->rust()->qr_request_get_sp_account());
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

void BwkQrEngine::prepareMusig(const QString &fingerprint, const QString &descriptor) {
    if (m_module->rust() == nullptr) {
        emit error(qr::QrError::RequestFailed, TR("keys-module-not-ready"));
        return;
    }
    m_cancelled = false;
    m_musig_fingerprint = fingerprint;
    m_musig_policy = descriptor;
    // The record must be durable before the request is shown: the worker persists it.
    auto *thread = QThread::create(
        [this, fingerprint = fingerprint.toStdString(), descriptor = descriptor.toStdString()] {
            auto prepared = ::qr_musig_prepare_material(fingerprint, descriptor);
            emit musigPrepareReady(reinterpret_cast<quintptr>(prepared.into_raw()));
        });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void BwkQrEngine::signMusig(const QString &fingerprint, const QString &psbt_v2_hex,
                            const QString &descriptor, const QHash<QString, KeyRouteKind> &routes) {
    if (m_module->rust() == nullptr) {
        emit error(qr::QrError::RequestFailed, TR("keys-module-not-ready"));
        return;
    }
    m_cancelled = false;
    m_musig_fingerprint = fingerprint;
    m_musig_policy = descriptor;
    m_musig_psbt = psbt_v2_hex;
    // Every required signer's session is allocated on disk before the request leaves.
    auto *thread = QThread::create(
        [this, fingerprint = fingerprint.toStdString(), descriptor = descriptor.toStdString(),
         psbt = psbt_v2_hex.toStdString(), routes = KeysModule::toBridgeRoutes(routes)]() mutable {
            auto signing =
                ::qr_musig_sign_material(fingerprint, descriptor, psbt, std::move(routes));
            emit musigSignReady(reinterpret_cast<quintptr>(signing.into_raw()));
        });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
    thread->start();
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
    m_cancelled = true;
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
        // A refused answer leaves the request outstanding: the scan goes on. A lost
        // MuSig2 session ends it: a later scan must not be decoded against it.
        if (decoded->error_kind() == QrErrorKind::MusigSession) {
            m_waiting = false;
        }
        emit error(toQrError(decoded->error_kind()), KeysModule::toQt(decoded->get_error()));
        return;
    }
    switch (decoded->kind()) {
    case QrResponseKind::MusigPrepared:
    case QrResponseKind::MusigSignedPsbt:
        // The cache accepts or refuses the answer off-thread; the request is consumed
        // either way.
        m_waiting = false;
        acceptMusig(std::move(decoded));
        return;
    default:
        break;
    }
    m_waiting = false;
    forwardResponse(*decoded);
}

void BwkQrEngine::acceptMusig(rust::Box<::QrResponse> decoded) {
    auto *thread = QThread::create(
        [this, decoded = decoded.into_raw(), fingerprint = m_musig_fingerprint.toStdString(),
         policy = m_musig_policy.toStdString(), psbt = m_musig_psbt.toStdString()]() mutable {
            auto accepted = ::qr_musig_accept(rust::Box<::QrResponse>::from_raw(decoded),
                                              fingerprint, policy, psbt);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            emit musigAccepted(accepted.is_ok, KeysModule::toQt(accepted.error),
                               accepted.musig_session, KeysModule::toQt(accepted.psbt_v2_hex));
        });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void BwkQrEngine::onMusigPrepareReady(quintptr material) {
    if (m_cancelled || m_module->rust() == nullptr) {
        // Back to Rust for its own drop: opaque boxes have no C++ destructor.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        rust::Box<::QrMusigPrepare>::from_raw(reinterpret_cast<::QrMusigPrepare *>(material));
        return;
    }
    auto preparation = rust::Box<::QrMusigPrepare>::from_raw(
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        reinterpret_cast<::QrMusigPrepare *>(material));
    issue(m_module->rust()->qr_request_prepare_musig(std::move(preparation)));
}

void BwkQrEngine::onMusigSignReady(quintptr material) {
    if (m_cancelled || m_module->rust() == nullptr) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        rust::Box<::QrMusigSign>::from_raw(reinterpret_cast<::QrMusigSign *>(material));
        return;
    }
    auto signing = rust::Box<::QrMusigSign>::from_raw(
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        reinterpret_cast<::QrMusigSign *>(material));
    issue(m_module->rust()->qr_request_sign_musig(std::move(signing)));
}

void BwkQrEngine::onMusigAccepted(bool is_ok, const QString &message, bool musig_session,
                                  const QString &psbt_v2_hex) {
    if (m_cancelled) {
        return;
    }
    if (!is_ok) {
        emit error(musig_session ? qr::QrError::MusigSession : qr::QrError::RequestFailed, message);
        return;
    }
    if (psbt_v2_hex.isEmpty()) {
        emit noncesPrepared();
        return;
    }
    emit signedPsbt(psbt_v2_hex);
}

void BwkQrEngine::forwardResponse(const ::QrResponse &decoded) {
    switch (decoded.kind()) {
    case QrResponseKind::Xpubs: {
        auto entries = decoded.xpubs();
        if (entries.empty()) {
            emit error(qr::QrError::RequestFailed, TR("keys-qr-wrong-kind"));
            return;
        }
        qr::QrDevice device{.vendor = KeysModule::toQt(decoded.vendor()),
                            .model = KeysModule::toQt(decoded.model()),
                            .version = KeysModule::toQt(decoded.version()),
                            .capabilities = decoded.capabilities()};
        emit xpubs(KeysModule::toQt(decoded.fingerprint()), toQrXpubs(entries), device);
        return;
    }
    case QrResponseKind::SpAccount:
        emit spAccount(KeysModule::toQt(decoded.descriptor()), toQrXpubs(decoded.xpubs()));
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
