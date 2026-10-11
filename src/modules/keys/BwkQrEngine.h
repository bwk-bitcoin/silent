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
    void getSpDescriptor() override;
    void sign(const QString &psbt_v2_hex, const QString &descriptor) override;
    void verifyAddress(const QString &descriptor, const QString &path,
                       const QString &address) override;
    void stop() override;

public slots:
    void response(const qr::QrResponse &response) override;

protected:
    // Hands the session's request on, or says why it could not be built.
    void issue(const QrMessageResult &message);
    // Forwards a validated response as the signal of its kind.
    void forwardResponse(const ::QrResponse &decoded);

private:
    // Non-owning: the keys module outlives every flow it opens.
    KeysModule *m_module = nullptr;
    bool m_waiting = false;
};

} // namespace keys
