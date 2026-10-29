#pragma once

#include "qr/QrTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <silent.h>

namespace qr {

// A QR protocol codec: turns the exchanges a caller asks for into request payloads
// (request()), and the payloads scanned back into their answer or an error. Framing
// and scanning the payloads is the transport engines' business. One exchange at a
// time: each request ends the one in progress.
class QrEngine : public QObject {
    Q_OBJECT

public:
    explicit QrEngine(QObject *parent = nullptr);
    // Each path is written m/...
    virtual void getXpubs(const QStringList &paths) = 0;
    // The silent payment account: its sp() descriptor, with its taproot and segwit
    // sub-accounts' xpubs.
    virtual void getSpAccount() = 0;
    virtual void sign(const QString &psbt_v2_hex, const QString &descriptor) = 0;
    // A cached MuSig preparation: public nonces for a fresh cache session, bound to
    // no transaction.
    virtual void prepareMusig(const QString &fingerprint, const QString &descriptor) = 0;
    // The first signature request of a prepared cache session: the attempt's sessions
    // are allocated durably before the request is shown. `routes` names every pending
    // signer's route, so the allocation binds all of them.
    virtual void signMusig(const QString &fingerprint, const QString &psbt_v2_hex,
                           const QString &descriptor,
                           const QHash<QString, KeyRouteKind> &routes) = 0;
    virtual void verifyAddress(const QString &descriptor, const QString &path,
                               const QString &address) = 0;
    // Ends the exchange: a late answer is dropped.
    virtual void stop() = 0;

signals:
    void request(const qr::QrRequest &request);
    // One xpub per path asked, in the order asked.
    void xpubs(const QString &fingerprint, const QList<qr::QrXpub> &xpubs,
               const qr::QrDevice &device);
    void spAccount(const QString &descriptor, const QList<qr::QrXpub> &xpubs);
    void signedPsbt(const QString &psbt_v2_hex);
    // The prepared answer checked out and is in the cache.
    void noncesPrepared();
    void addressVerified(bool matched);
    void error(qr::QrError kind, const QString &message);

public slots:
    virtual void response(const qr::QrResponse &response) = 0;
};

} // namespace qr
