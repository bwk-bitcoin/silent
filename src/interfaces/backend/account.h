#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "service.h"
#include "types.h"

namespace backend {

class IAccountBackend : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~IAccountBackend() override = default;

    [[nodiscard]] virtual auto id() const -> QString = 0;

    [[nodiscard]] virtual auto name() const -> QString = 0;

    [[nodiscard]] virtual auto network() const -> Network = 0;

    [[nodiscard]] virtual auto addressKinds() const -> QList<AddressKind> = 0;

    [[nodiscard]] virtual auto addresses() const -> QList<Address> = 0;

    [[nodiscard]] virtual auto addressVerification(const QString &address) const
        -> std::optional<AddressVerification> = 0;

public slots:
    virtual void newAddress(backend::RequestId request_id, QString kind) = 0;

    // The account's backup, its own format, answered by backupCreated().
    virtual void backup(backend::RequestId request_id) = 0;

    // What `backup` holds, an encrypted one opened with one of `xpubs` (an xpub, or the sp()
    // descriptor a key gives, whose scan secret opens an SP backup) or with `mnemonic` (empty
    // for none): answered by backupInspected(), or by backupLocked() when none opens it.
    virtual void inspectBackup(backend::RequestId request_id, QByteArray backup, QStringList xpubs,
                               QByteArray mnemonic) = 0;

    // Makes the account `backup` holds, opened as inspectBackup() opens it, named `name`,
    // on `network` when the backup does not tell its own.
    virtual void restore(backend::RequestId request_id, QByteArray backup, QStringList xpubs,
                         QByteArray mnemonic, QString name, backend::Network network) = 0;

signals:
    void addressesChanged();

    void addressCreated(backend::RequestId request_id, backend::Address address);

    // `excluded`: the keys the backup is not encrypted to, whose holders cannot open it.
    void backupCreated(backend::RequestId request_id, QByteArray backup, QStringList excluded);

    void backupInspected(backend::RequestId request_id, backend::BackupInfo info);

    // The backup is encrypted and none of the keys given opens it: the derivation paths of
    // the xpubs that may, written m/...
    void backupLocked(backend::RequestId request_id, QStringList derivation_paths);

    void restored(backend::RequestId request_id);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IAccountBackend, "dev.silent.backend.IAccountBackend/1.0")
