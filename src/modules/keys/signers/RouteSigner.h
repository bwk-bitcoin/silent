#pragma once

#include <QObject>
#include <QString>

#include <interfaces/signer.h>
#include <interfaces/types.h>

#include <cstdint>

struct KeyInfo;
enum class KeyRouteKind : std::uint8_t;

namespace keys {

class KeysModule;

// Stands for the hot, QR or SD card route of a registered key, whichever the module
// picked as the key's best usable route, in the key list accounts read. It answers
// info and cached xpubs; signing and address checks go through IKeys, whose flow
// drives the route itself. All four immutable accessors are fixed at construction;
// the module drops and rebuilds the instance rather than mutating it when the key's
// chosen route changes.
class RouteSigner final : public ISigner {
    Q_OBJECT

public:
    RouteSigner(KeysModule *module, const KeyInfo &key, KeyRouteKind route,
                QObject *parent = nullptr);
    ~RouteSigner() override = default;

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

    // The route this instance serves. The module compares this against a key's
    // freshly picked route to decide whether to reuse or rebuild.
    [[nodiscard]] auto route() const -> KeyRouteKind;

signals:
    // Queued to itself so a result never lands before the caller holds the ReqId.
    void deferredInfo(ReqId req_id);
    void deferredInitialized(ReqId req_id);
    void deferredXpub(ReqId req_id, QString xpub);
    // Answers registerDescriptor(); descriptorIsRegistered() has its own signal below,
    // since ISigner treats "is it registered" and "register it" as distinct events.
    void deferredRegistered(ReqId req_id, bool registered);
    void deferredIsRegistered(ReqId req_id, bool registered);
    void deferredError(ReqId req_id, QString message);

public slots:
    void onDeferredInfo(ReqId req_id);
    void onDeferredInitialized(ReqId req_id);
    void onDeferredXpub(ReqId req_id, QString xpub);
    void onDeferredRegistered(ReqId req_id, bool registered);
    void onDeferredIsRegistered(ReqId req_id, bool registered);
    void onDeferredError(ReqId req_id, QString message);

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    QString m_alias;
    KeyRouteKind m_route{};
};

} // namespace keys
