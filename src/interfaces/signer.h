#pragma once

#include <optional>

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>

#include <interfaces/types.h>

class ISigner : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~ISigner() override = default;

    [[nodiscard]] virtual bool implemented() const = 0;
    // Immutable, set at construction. None of the four identifies the signer:
    // id() is module-defined with no uniqueness guarantee and fingerprint() is
    // shared by every device holding the seed. Address a signer by the
    // instanceId that Host::signers() pairs with it.
    [[nodiscard]] virtual QString id() const = 0;
    [[nodiscard]] virtual QString fingerprint() const = 0;
    [[nodiscard]] virtual QString walletName() const = 0;
    // Immutable too: a signer whose state changes is dropped and registered
    // again, so a reader never sees it change underneath. Requests on anything
    // but Ready answer with error(). stateDetail() says why, in one line the UI
    // can show (a pairing code, "app is not open", "wrong network").
    [[nodiscard]] virtual plugin::SignerState state() const = 0;
    [[nodiscard]] virtual QString stateDetail() const = 0;

    virtual ReqId init() = 0;
    virtual ReqId info() = 0;
    virtual ReqId getXpub(const QString &path) = 0;
    virtual ReqId isDescriptorRegistered(const QString &descriptor) = 0;
    virtual ReqId registerDescriptor(const QString &descriptor) = 0;
    virtual ReqId signWithDescriptor(const QString &psbt_base64, const QString &descriptor) = 0;
    virtual ReqId raw(const QByteArray &request) = 0;

signals:
    void initialized(ReqId req_id);
    void info(ReqId req_id, QMap<QString, QString> info);
    void xpub(ReqId req_id, QString xpub);
    void descriptorRegistered(ReqId req_id, bool registered);
    void descriptorIsRegistered(ReqId req_id, bool registered);
    // NOLINTNEXTLINE(readability-identifier-naming)
    void signed_(ReqId req_id, QString psbt_base64);
    void raw(ReqId req_id, QByteArray response);
    void error(std::optional<ReqId> req_id, QString message);
};

Q_DECLARE_INTERFACE(ISigner, "dev.silent.ISigner/1.0")
