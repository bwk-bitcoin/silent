#pragma once

#include <QObject>
#include <QString>

#include <interfaces/signer.h>
#include <interfaces/types.h>

struct KeyInfo;

namespace keys {

class KeysModule;

// Stand-in for a registered key with no usable route right now: every device seen
// on it is disconnected, it has no hot secret, and no QR or SD card route is
// configured. Answers info() and cached xpubs from the record like any other
// signer for this key; every signing, registration and address-display request
// fails with a clear reason instead of a device round trip.
class OfflineSigner final : public ISigner {
    Q_OBJECT

public:
    OfflineSigner(KeysModule *module, const KeyInfo &key, QObject *parent = nullptr);
    ~OfflineSigner() override = default;

    [[nodiscard]] bool implemented() const override;
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString fingerprint() const override;
    [[nodiscard]] QString walletName() const override;
    [[nodiscard]] QString brand() const override;
    [[nodiscard]] QString model() const override;
    [[nodiscard]] QString serial() const override;
    [[nodiscard]] plugin::SignerState state() const override;
    [[nodiscard]] QString stateDetail() const override;

    ReqId init() override;
    ReqId info() override;
    ReqId getXpub(const QString &path) override;
    ReqId isDescriptorRegistered(const QString &descriptor) override;
    ReqId registerDescriptor(const QString &descriptor) override;
    ReqId signWithDescriptor(const QString &psbt_base64, const QString &descriptor) override;
    ReqId displayAddress(const QString &descriptor, const QString &path,
                         const QString &address) override;
    ReqId raw(const QByteArray &request) override;

signals:
    // Queued to itself so a result never lands before the caller holds the ReqId.
    void deferredInfo(ReqId req_id);
    void deferredIsRegistered(ReqId req_id, bool registered);
    void deferredXpub(ReqId req_id, QString xpub);
    void deferredError(ReqId req_id, QString message);

public slots:
    void onDeferredInfo(ReqId req_id);
    void onDeferredIsRegistered(ReqId req_id, bool registered);
    void onDeferredXpub(ReqId req_id, QString xpub);
    void onDeferredError(ReqId req_id, QString message);

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    QString m_alias;
};

} // namespace keys
