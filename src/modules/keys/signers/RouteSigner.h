#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <interfaces/signer.h>
#include <interfaces/types.h>

#include <cstdint>

struct KeyInfo;
enum class KeyRouteKind : std::uint8_t;

namespace keys {

class KeysModule;

// Serves the hot, QR or SD card route of a registered key, whichever the module
// picked as the key's best usable route. All four immutable accessors are fixed at
// construction; the module drops and rebuilds the instance rather than mutating it
// when the key's chosen route changes.
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

    // Called by the module once its modal flow for an interactive route ends.
    void deliverSigned(ReqId req_id, const QString &psbt_hex);
    void deliverDisplayed(ReqId req_id, bool displayed);
    void deliverError(ReqId req_id, const QString &message);

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
    // The module drives the QR and SD card flows, which need modals.
    void routeSignRequested(ReqId req_id, QString fingerprint, QString psbt_v2_hex,
                            QString descriptor);
    void routeDisplayRequested(ReqId req_id, QString fingerprint, QString descriptor, QString path,
                               QString address);

public slots:
    void onDeferredInfo(ReqId req_id);
    void onDeferredInitialized(ReqId req_id);
    void onDeferredXpub(ReqId req_id, QString xpub);
    void onDeferredRegistered(ReqId req_id, bool registered);
    void onDeferredIsRegistered(ReqId req_id, bool registered);
    void onDeferredError(ReqId req_id, QString message);
    // Matched against the module's broadcast KeysModule::hotSigned signal by req_id:
    // every RouteSigner instance receives it, so only the id this instance is
    // waiting on is acted on.
    void onHotSigned(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error);
    // Called by the module once its password prompt for `req_id` resolves. An empty
    // password means the user cancelled the modal.
    void onPasswordResolved(ReqId req_id, QString password);

protected:
    void signHot(ReqId req_id, const QString &psbt_v2_hex, const QString &descriptor);

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    QString m_alias;
    KeyRouteKind m_route{};
    // A hot-sign request's PSBT, kept from signHot() until the module answers
    // through hotSigned(): membership doubles as "is this req_id mine" when that
    // broadcast signal lands.
    QHash<ReqId, QString> m_pending_signs;
    QHash<ReqId, QString> m_display_addresses;
};

} // namespace keys
