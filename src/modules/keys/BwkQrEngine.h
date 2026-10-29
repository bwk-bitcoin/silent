#pragma once

#include "qr/QrEngine.h"

#include <silent.h>

namespace keys {

class KeysModule;

// The bwk-qr protocol codec, on the keys module's QR session: the session issues each
// request and checks the answer against it.
class BwkQrEngine final : public qr::QrEngine {
    Q_OBJECT

public:
    explicit BwkQrEngine(KeysModule *module, QObject *parent = nullptr);
    void getXpubs(const QStringList &paths) override;
    void getSpAccount() override;
    void sign(const QString &psbt_v2_hex, const QString &descriptor) override;
    void prepareMusig(const QString &fingerprint, const QString &descriptor) override;
    void signMusig(const QString &fingerprint, const QString &psbt_v2_hex,
                   const QString &descriptor, const QHash<QString, KeyRouteKind> &routes) override;
    void verifyAddress(const QString &descriptor, const QString &path,
                       const QString &address) override;
    void stop() override;

signals:
    // The workers' results, queued back to the GUI thread from the exchange threads.
    void musigPrepareReady(quintptr material);
    void musigSignReady(quintptr material);
    void musigAccepted(bool is_ok, const QString &message, bool musig_session,
                       const QString &psbt_v2_hex);

public slots:
    void response(const qr::QrResponse &response) override;
    // Reclaims the worker's box and issues the wire request.
    void onMusigPrepareReady(quintptr material);
    void onMusigSignReady(quintptr material);
    void onMusigAccepted(bool is_ok, const QString &message, bool musig_session,
                         const QString &psbt_v2_hex);

protected:
    // Hands the session's request on, or says why it could not be built.
    void issue(const QrMessageResult &message);
    // Forwards a validated response as the signal of its kind.
    void forwardResponse(const ::QrResponse &decoded);
    // Runs the cache accept for a decoded cached answer on a worker, and queues the
    // result back.
    void acceptMusig(rust::Box<::QrResponse> decoded);

private:
    // Non-owning: the keys module outlives every flow it opens.
    KeysModule *m_module = nullptr;
    bool m_waiting = false;
    // A stopped engine drops the results its workers still deliver.
    bool m_cancelled = false;
    // The cached exchange on hand: who it is for, and the transaction it signs.
    QString m_musig_fingerprint;
    QString m_musig_policy;
    QString m_musig_psbt;
};

} // namespace keys
