#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <interfaces/signer.h>
#include <interfaces/types.h>

struct HwiDevice;
struct HwiEvent;
class HwiModule;

// One signing device, as ISigner. Every device the service reports gets one, usable
// or not: state() says which, so a picker can show a locked device instead of hiding
// it. All four immutable accessors are fixed at construction; the module drops and
// re-registers the instance when the device changes, rather than mutating them.
class HwiSigner final : public ISigner {
    Q_OBJECT

public:
    HwiSigner(HwiModule *module, const HwiDevice &device, QObject *parent = nullptr);
    ~HwiSigner() override = default;

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

    // Called by the module when a notification carries one of this signer's request ids.
    void deliver(const HwiEvent &event);

    [[nodiscard]] QString kind() const;
    [[nodiscard]] QString version() const;
    // Whether the device record still describes this signer. A change means a new
    // instance, because the accessors are immutable.
    [[nodiscard]] bool matches(const HwiDevice &device) const;
    // Whether a device of `kind` (the service's kind text) is registered by its stored
    // proof alone: a Ledger, which keeps no wallet list. Every other kind is asked.
    [[nodiscard]] static auto provesRegistrationByHmac(const QString &kind) -> bool;
    // Whether a device of `kind` accepts a registration before its user approved it on
    // screen: a Coldcard, whose enroll request answers at once with the story still up.
    // Its registration is confirmed by asking until the device lists the descriptor. A
    // refusal on the device reaches the host no other way, so a descriptor still not
    // listed after 5 minutes answers as refused.
    [[nodiscard]] static auto registersBeforeApproval(const QString &kind) -> bool;

signals:
    // Queued to itself so a result never lands before the caller has the ReqId.
    void deferredInfo(ReqId req_id);
    void deferredInitialized(ReqId req_id);
    void deferredError(ReqId req_id, QString message);
    void deferredIsRegistered(ReqId req_id, bool registered);

private slots:
    void onDeferredInfo(ReqId req_id);
    void onDeferredInitialized(ReqId req_id);
    void onDeferredError(ReqId req_id, QString message);
    void onDeferredIsRegistered(ReqId req_id, bool registered);
    // Asks again whether each registration still waiting on its approval is listed.
    void onConfirmTick();

protected:
    enum class Request { Xpub, IsRegistered, Register, Sign, DisplayAddress };

    // Allocates a request id and dispatches, or defers an error when the device cannot
    // take it. Either way the caller gets an id and an answer on a signal.
    ReqId dispatch(Request request, const QString &first, const QString &second = QString());
    [[nodiscard]] bool ready() const;
    // The stored registration proof of `descriptor` on this device's key, empty when
    // none (or no keys module).
    [[nodiscard]] auto storedHmac(const QString &descriptor) const -> QString;
    // Stores the proof a registration of `descriptor` answered with, a warning when the
    // keys module refuses it.
    void storeHmac(const QString &descriptor, const QString &hmac) const;

private:
    // A registration the device accepted and its user has still to approve.
    struct PendingApproval {
        ReqId request = 0; // the registerDescriptor() request it answers
        QString descriptor;
        QElapsedTimer since;
        ReqId check = 0; // the isDescriptorRegistered request in flight, 0 when none
    };
    // Answers a pending approval and forgets it.
    void finishApproval(qsizetype index, bool registered);
    // The registerDescriptor() request whose check `req_id` is, forgetting its pending
    // approval; `req_id` itself when it is no check.
    [[nodiscard]] auto takeApprovalRequest(ReqId req_id) -> ReqId;
    void removeApproval(qsizetype index);

    HwiModule *m_module = nullptr;
    QString m_id;
    QString m_kind;
    QString m_fingerprint;
    QString m_version;
    QString m_detail;
    plugin::SignerState m_state = plugin::SignerState::Unsupported;
    QHash<ReqId, QString> m_display_addresses;
    // The descriptor of each registration in flight, to store the proof it answers with.
    QHash<ReqId, QString> m_register_descriptors;
    QList<PendingApproval> m_pending_approvals;
    // Runs while m_pending_approvals is not empty.
    QTimer m_confirm_timer;
};
