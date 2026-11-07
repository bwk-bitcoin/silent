#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

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

    virtual void backup(backend::RequestId request_id, QByteArray password) = 0;

    virtual void inspectBackup(backend::RequestId request_id, QByteArray backup,
                               QByteArray password) = 0;

    virtual void restore(backend::RequestId request_id, QByteArray backup, QByteArray password) = 0;

signals:
    void addressesChanged();

    void addressCreated(backend::RequestId request_id, backend::Address address);

    void backupCreated(backend::RequestId request_id, QByteArray backup);

    void backupInspected(backend::RequestId request_id, backend::BackupInfo info);

    void restored(backend::RequestId request_id);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IAccountBackend, "dev.silent.backend.IAccountBackend/1.0")
