#pragma once

#include <QHash>
#include <QObject>
#include <QString>

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

signals:
    // Queued to itself so a result never lands before the caller has the ReqId.
    void deferredInfo(ReqId req_id);
    void deferredInitialized(ReqId req_id);
    void deferredError(ReqId req_id, QString message);

private slots:
    void onDeferredInfo(ReqId req_id);
    void onDeferredInitialized(ReqId req_id);
    void onDeferredError(ReqId req_id, QString message);

protected:
    enum class Request { Xpub, IsRegistered, Register, Sign, DisplayAddress };

    // Allocates a request id and dispatches, or defers an error when the device cannot
    // take it. Either way the caller gets an id and an answer on a signal.
    ReqId dispatch(Request request, const QString &first, const QString &second = QString());
    [[nodiscard]] bool ready() const;

private:
    HwiModule *m_module = nullptr;
    QString m_id;
    QString m_kind;
    QString m_fingerprint;
    QString m_version;
    QString m_detail;
    plugin::SignerState m_state = plugin::SignerState::Unsupported;
    QHash<ReqId, QString> m_display_addresses;
};
