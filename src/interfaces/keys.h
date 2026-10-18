#pragma once

#include <optional>

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <interfaces/types.h>
#include <silent.h>

class ISigner;

// One registered key, as an account sees it. `signer` is the key's best usable route:
// the device's own ISigner for a connected USB route, a signer supplied by the keys
// module for a hot, QR or SD card route, and a stand-in that answers only info and
// cached xpubs when no route is usable. `can_sign` is false for that stand-in.
// `descriptor_route` is the route spDescriptor() fetches over (QR or hot), Unknown
// when the key has neither. `can_show_address` is true when the key has a USB or QR
// route, a device with a screen verifyAddress() can show an address on. `route` is the
// key's first route, connected or not, Unknown when it has none. `on_device` is true
// when the key's best route is its connected USB device: signOnDevice() can sign
// with it without a window.
struct KeyEntry {
    QString fingerprint;
    QString alias;
    bool can_sign{};
    ISigner *signer{};
    KeyRouteKind descriptor_route = KeyRouteKind::Unknown;
    bool can_show_address{};
    KeyRouteKind route = KeyRouteKind::Unknown;
    // Signs MuSig2: a hot key, or a device reporting the MuSig2 capability.
    bool can_musig{};
    bool on_device{};
};

// The keys module, as accounts and plugins see it. An account never picks a signer:
// it hands over a PSBT and the module owns the picker and every route step. Each of
// sign(), verifyAddress(), spDescriptor() and xpub() runs one modal flow (xpub() none
// when cached) and returns once its window has closed; it answers exactly once, with
// its result, error() or cancelled(). A MuSig2 PSBT takes two sign() calls: the
// first gathers every cosigner's nonce, the second their partial signatures.
class IKeys : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IKeys() override = default;

    // Registered keys for `network`, in a stable order. GUI thread only.
    [[nodiscard]] virtual QList<KeyEntry> keys(Network network) const = 0;
    // Allocates a request id. Callers must call this before every request below,
    // store the id, then call the request: an answer may arrive during the call.
    virtual auto newRequest() -> ReqId = 0;
    // Each request below names the network of the keys it needs (the account's), which
    // need not be the one the Keys tab shows.
    // Signs the PSBT with every key the descriptors need, in the order the module
    // decides, or with the key `fingerprint` alone when it is set (no signer picker).
    // `wallet_name` names the files of an SD card route.
    // One round per call. A PSBT answered with only MuSig2 nonces added is success
    // (signed_), the caller calls sign() again for the next round.
    virtual void sign(ReqId req_id, Network network, const QString &psbt_v2_hex,
                      const QStringList &descriptors, const QString &wallet_name,
                      const QString &fingerprint) = 0;
    // Signs the PSBT with the key `fingerprint` on its connected USB device (a key
    // whose entry is `on_device`), opening no window: for a caller that shows the wait
    // itself. Answers as sign() does, signed_() or error(), never cancelled().
    virtual void signOnDevice(ReqId req_id, Network network, const QString &psbt_v2_hex,
                              const QString &descriptor, const QString &fingerprint) = 0;
    // Shows `address` on the device holding the key `descriptor` belongs to.
    virtual void verifyAddress(ReqId req_id, Network network, const QString &descriptor,
                               const QString &path, const QString &address) = 0;
    // Fetches the sp() descriptor from the key, over its best route. The only
    // descriptor a key ever hands out: see the SP exception in the design.
    virtual void spDescriptor(ReqId req_id, Network network, const QString &fingerprint) = 0;
    // Fetches the key's xpub at `path` (an account's segwit or taproot sub-account):
    // from the key's cache when it holds one, else over the key's route. Answers
    // xpubReady(), error() or cancelled().
    virtual void xpub(ReqId req_id, Network network, const QString &fingerprint,
                      const QString &path) = 0;
    // The key's xpub at `path` when its cache holds one, else empty: no modal, no
    // request, so a caller can skip xpub() and the window it opens.
    [[nodiscard]] virtual auto cachedXpub(Network network, const QString &fingerprint,
                                          const QString &path) const -> QString = 0;
    // The proof the device returned when `descriptor` was registered on the key
    // `fingerprint` (Ledger's wallet HMAC, hex), empty when none is stored. No request.
    [[nodiscard]] virtual auto registrationHmac(Network network, const QString &fingerprint,
                                                const QString &descriptor) const -> QString = 0;
    // Stores it, recording `descriptor` as registered with the key. False when the key
    // is unknown or `hmac` is not 32 bytes of hex.
    virtual auto storeRegistrationHmac(Network network, const QString &fingerprint,
                                       const QString &descriptor, const QString &hmac) -> bool = 0;
    // Records that `descriptor` is registered with this key, and registers it on the
    // devices that need it.
    virtual void registerDescriptor(ReqId req_id, Network network, const QString &fingerprint,
                                    const QString &descriptor) = 0;

signals:
    // NOLINTNEXTLINE(readability-identifier-naming)
    void signed_(ReqId req_id, QString psbt_v2_hex);
    // A refusal (verified=false) is the device answering that the address does
    // not match, not a transport failure: it never comes through error().
    void verified(ReqId req_id, bool verified);
    void spDescriptorReady(ReqId req_id, QString descriptor, QString fingerprint);
    void xpubReady(ReqId req_id, QString xpub, QString fingerprint, QString path);
    void descriptorRegistered(ReqId req_id, QString fingerprint);
    // A failure the caller reports, once. `kind` is MusigSession when a key lost its
    // MuSig2 session: drop every nonce and partial signature and start again from
    // round 1. Other for everything else.
    void error(std::optional<ReqId> req_id, QString message,
               plugin::SignError kind = plugin::SignError::Other);
    // The user closed the flow, or it already showed why there is no answer: the
    // caller shows nothing.
    void cancelled(ReqId req_id);
};

Q_DECLARE_INTERFACE(IKeys, "dev.silent.IKeys/1.0")
