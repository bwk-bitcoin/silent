#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "service.h"
#include "types.h"

namespace backend {

// An account as the generic account UI sees it: its identity, the addresses it receives on,
// its backup and the restore of one. A module's restore target is one with no account behind
// it: empty identity and addresses, only inspectBackup() and restore() served.
class IAccountBackend : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~IAccountBackend() override = default;

    // The account's instance id, the `account` IServiceProvider::service() was asked for.
    // Empty on a restore target.
    [[nodiscard]] virtual auto id() const -> QString = 0;

    // What the user is shown. A built-in account is named by its id; empty from the host's
    // unsupported service and on a restore target.
    [[nodiscard]] virtual auto name() const -> QString = 0;

    // The network the account is on, the one IServiceProvider::service() was asked for.
    [[nodiscard]] virtual auto network() const -> Network = 0;

    // Every kind of address the account receives on, in the order Receive shows them (a
    // built-in SP account: "sp" first, then "segwit" and "taproot" when it has those
    // sub-accounts; a Multikey one: "taproot"). Empty when it receives on none.
    [[nodiscard]] virtual auto addressKinds() const -> QList<AddressKind> = 0;

    // The receive addresses the account handed out, the reusable ones included, not the ones
    // only watched past them. A built-in account lists the reusable one first, then each
    // kind's by derivation index (from 1), oldest first. Changes with addressesChanged().
    [[nodiscard]] virtual auto addresses() const -> QList<Address> = 0;

    // How to show `address` on a signing device, std::nullopt when it is none of addresses().
    [[nodiscard]] virtual auto addressVerification(const QString &address) const
        -> std::optional<AddressVerification> = 0;

    // What the account's backup holds and what opens it, for its own kind.
    [[nodiscard]] virtual auto backupDescription() const -> BackupDescription = 0;

public slots:
    // Hands out an address of the AddressKind::id `kind`, answered by addressCreated(): the
    // same static one each time for a reusable kind, else the next unused one, persisted.
    virtual void newAddress(backend::RequestId request_id, QString kind) = 0;

    // The account's backup, its own format, answered by backupCreated(). A built-in one is
    // a BIP139 document encrypted with BIP138 to the account's keys (a Multikey account's
    // policy keys, an SP account's scan secret).
    virtual void backup(backend::RequestId request_id) = 0;

    // What `backup` holds, an encrypted one opened with one of `xpubs` (an xpub, or the sp()
    // descriptor a key gives, whose scan secret opens an SP backup) or with `mnemonic` (empty
    // for none): answered by backupInspected(), or by backupLocked() when none opens it.
    // The built-in accounts read, in this order, BIP138(BIP139), BIP138(descriptor), a plain
    // BIP139 document and a plain descriptor, the plain ones needing no key; anything else
    // answers error(). A restore target answers error() for a backup of another kind of
    // account; an open built-in account reads either kind.
    virtual void inspectBackup(backend::RequestId request_id, QByteArray backup, QStringList xpubs,
                               QByteArray mnemonic) = 0;

    // Makes the account `backup` holds, opened as inspectBackup() opens it, named `name`,
    // on `network` when the backup does not tell its own: answered by restored(). A built-in
    // one refuses with error() an existing name, and a restore through an open account's
    // service; nothing stays of a failed restore.
    virtual void restore(backend::RequestId request_id, QByteArray backup, QStringList xpubs,
                         QByteArray mnemonic, QString name, backend::Network network) = 0;

signals:
    // addresses() changed.
    void addressesChanged();

    // Answers newAddress(): the address handed out.
    void addressCreated(backend::RequestId request_id, backend::Address address);

    // Answers backup(): `backup`, the bytes to save as they are (a built-in one as a .bip138
    // file). `excluded`: the key expressions the backup is not encrypted to, whose holders
    // cannot open it; empty when every key opens it.
    void backupCreated(backend::RequestId request_id, QByteArray backup, QStringList excluded);

    // Answers inspectBackup(): what the backup holds and what restoring it needs.
    void backupInspected(backend::RequestId request_id, backend::BackupInfo info);

    // Answers inspectBackup() when the backup is encrypted and none of the keys given opens
    // it: the derivation paths of the xpubs that may, written m/... (an SP backup's names
    // the BIP352 scan path, m/352'/coin'/account'/1'/0).
    void backupLocked(backend::RequestId request_id, QStringList derivation_paths);

    // Answers restore(): the account is made, and listed by its module.
    void restored(backend::RequestId request_id);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::IAccountBackend, "dev.silent.backend.IAccountBackend/1.0")
