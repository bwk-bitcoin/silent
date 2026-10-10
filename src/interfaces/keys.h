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
struct KeyEntry {
    QString fingerprint;
    QString alias;
    bool can_sign{};
    ISigner *signer{};
};

// The keys module, as accounts and plugins see it. An account never picks a signer:
// it hands over a PSBT and the module owns the picker and every route step. Each of
// sign(), verifyAddress() and spDescriptor() runs one modal flow and returns once its
// window has closed; it answers exactly once, with its result, error() or cancelled().
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
    // Signs the PSBT with every key the descriptors need, in the order the module
    // decides. `wallet_name` names the files of an SD card route.
    virtual void sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors,
                      const QString &wallet_name) = 0;
    // Shows `address` on the device holding the key `descriptor` belongs to.
    virtual void verifyAddress(ReqId req_id, const QString &descriptor, const QString &path,
                               const QString &address) = 0;
    // Fetches the sp() descriptor from the key, over its best route. The only
    // descriptor a key ever hands out: see the SP exception in the design.
    virtual void spDescriptor(ReqId req_id, const QString &fingerprint) = 0;
    // Records that `descriptor` is registered with this key, and registers it on the
    // devices that need it.
    virtual void registerDescriptor(ReqId req_id, const QString &fingerprint,
                                    const QString &descriptor) = 0;

signals:
    // NOLINTNEXTLINE(readability-identifier-naming)
    void signed_(ReqId req_id, QString psbt_v2_hex);
    // A refusal (verified=false) is the device answering that the address does
    // not match, not a transport failure: it never comes through error().
    void verified(ReqId req_id, bool verified);
    void spDescriptorReady(ReqId req_id, QString descriptor, QString fingerprint);
    void descriptorRegistered(ReqId req_id, QString fingerprint);
    // A failure the caller reports, once.
    void error(std::optional<ReqId> req_id, QString message);
    // The user closed the flow, or it already showed why there is no answer: the
    // caller shows nothing.
    void cancelled(ReqId req_id);
};

Q_DECLARE_INTERFACE(IKeys, "dev.silent.IKeys/1.0")
