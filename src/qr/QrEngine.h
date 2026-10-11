#pragma once

#include "qr/QrTypes.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

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
    virtual void getSpDescriptor() = 0;
    virtual void sign(const QString &psbt_v2_hex, const QString &descriptor) = 0;
    virtual void verifyAddress(const QString &descriptor, const QString &path,
                               const QString &address) = 0;
    // Ends the exchange: a late answer is dropped.
    virtual void stop() = 0;

signals:
    void request(const qr::QrRequest &request);
    // One xpub per path asked, in the order asked.
    void xpubs(const QString &fingerprint, const QList<qr::QrXpub> &xpubs,
               const qr::QrDevice &device);
    void spDescriptor(const QString &descriptor);
    void signedPsbt(const QString &psbt_v2_hex);
    void addressVerified(bool matched);
    void error(qr::QrError kind, const QString &message);

public slots:
    virtual void response(const qr::QrResponse &response) = 0;
};

} // namespace qr
