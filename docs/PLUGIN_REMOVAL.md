# External plugin removal

Settings offers Remove for every installed external plugin, including disabled,
incompatible and failed-to-load plugins. Removal uses installed metadata and does
not construct an unloaded plugin. Built-in plugins cannot be removed.

Confirmation names the plugin and warns that its installed bundle, saved state and
accounts are deleted. Back leaves installation, grants, runtime and storage alone.
There is no keep-data choice. Disable preserves private data.

## Native shutdown contract

The current root IID is IPlugin/12.0; IModule remains at 3.0. The shutdown contract,
introduced with IPlugin/9.0 and IModule/3.0, uses `shutdown(req_id)` and
`shutdownFinished(req_id, error)`. ABI 9, 10 and 11 libraries are now incompatible and can be
removed without loading them. The host records the request before dispatch and
receives completion through a queued connection, outside the native call stack.
The default implementation refuses shutdown. A removed registration or tab is not
completion.

All plugin-owned asynchronous work belongs to the returned modules. Root/module
constructors stay idle until host activation, including during admission failure.
Register an instance before publishing long-lived tabs/settings. Host rejects
unregistered or closing owners and retains the publishing module on each UI entry.
Wizards are synchronous, including destruction of their contents before openWizard
returns; shutdown must refuse or wait while any native setup frame remains active.

On shutdown a module must reject new work and drain every worker, backend, pending
write and already-closing instance it owns. Existing caller-scoped IStorage handles
remain available for accepted work and final backend flushes. New business requests,
host handles and instance registrations are refused. Modules enforce their own work
admission cooperatively. Success means no owned native work can outlive plugin
destruction. Join native workers before reporting success, including native
thread-local destruction. Cancel timers and retire queued callbacks. Never report
success merely because cancellation was requested.

The host withdraws feed sources and runtime UI without changing selected source
IDs, including the single configured fee provider. Its recommendations become
unavailable, not replaced by another provider. Once every
module reports successful shutdown the host revokes caller handles, waits for accepted host-owned storage
and transaction jobs, destroys deferred plugin UI, and unloads the library.

This is a cooperative in-process contract. It does not confine malicious native
code, prove compliance by an arbitrary library, or implement an OS sandbox.

## Failure and retry

Runtime availability is withdrawn when the action is accepted. Before unloading
or erasing data the host durably disables the plugin and records pending removal
in encrypted application state. Pending removal prevents enable and reimport,
including after restart. Save failure retains the library and data, withdraws
runtime access in this session and requires retry. If that checkpoint could not
be persisted, disabled state after restart cannot be guaranteed.

Shutdown refusal retains the mapped library and existing storage handles for retry.
The host never treats a failed module response as permission to unload. Unload failure retains data and the
bundle. Both are reported and may be retried. No failure is reported as removal.
Failed Disable has a separate Retry disable action that keeps saved data. It never
requires selecting destructive Remove to finish an ordinary runtime close.

The durable journal has monotonic Stopping, PrivateData, Bundle and Bookkeeping
phases. Every application-state mutation loads and saves under the same persistent
interprocess lock, including ordinary preferences and grants. Checked import
admission and bundle publication hold that lock together. Atomic replacement alone
does not protect against a stale snapshot from another host.

Retry never downgrades a phase. Stopping permits existing authenticated
sessions to finish shutdown persistence; new acquisition is refused by Rust as well
as the host. The first transition to PrivateData rotates a persisted storage epoch.
Every external operation checks its captured epoch, current grant and phase. Epochs
survive final bookkeeping and same-hash reinstall, so old sessions never revive.
An ordinary upgrade without removal preserves the epoch and stored data.
Permission prompts and Settings access views capture that same durable epoch when
they open. Every grant save checks the captured epoch and refuses any pending
removal under the interprocess lock. An old prompt cannot restore a grant after
removal or same-hash reinstall. A rejected save shows an error and grants nothing;
Settings keeps stale rows disabled until an explicit reload. A fresh request for
the new installation may approve its hash-specific grants. An I/O failure after
atomic replacement may leave a complete answer on disk; the error does not claim
otherwise. A deliberate reload reads the saved answers before another change.
After unload, PrivateData fences all external storage publication. Rust verifies
that checkpoint under the existing storage operation/publication barriers,
revokes matching sessions and atomically replaces the complete private tree with
an empty tree marked already migrated. Either the previous complete tree or the
empty complete tree survives a failed replacement, never a subset of accounts.
The encrypted empty marker and lock sentinel may remain; they contain no account
payload. The legacy blob migration source and any interrupted staging file are then
deleted. A legacy deletion
failure explicitly reports that the tree is already clear and keeps the bundle
and pending-removal marker for retry. The migration marker prevents that source
from resurrecting accounts after restart.
Deletion retries sync the containing directory even when the file is already absent;
unlink success alone does not complete a failed durability checkpoint.

Lock order is the process-local storage operation barrier, publication barrier,
app-state filesystem lock, then the nonblocking private-root lock. External storage
keeps authorization locked through tree publication. Synchronous cleanup keeps it
through its durable stages; no app-state lock spans asynchronous native shutdown.

Reserved storage roots were never private IStorage roots. An encrypted tree owned
by another ID, including a native case-insensitive alias, is protected rather than
erased. Only the removed ID's isolated legacy source and bundle are cleaned there.
An unverifiable tree owner is an explicit cleanup failure, not permission to erase.
Absent trees are not created or claimed during cleanup. Unpublished staging files
are removed without creating an owner marker; a readable foreign owner is protected.

The Bundle phase is persisted only after private cleanup succeeds. The installed
bundle and its staging file are then removed. Bookkeeping records that data and
bundle are gone before final grant cleanup. Finally the
host clears that plugin's enabled entry, all of its hash-specific grants/history
and the removal marker with checked persistence. If that save fails, the error
states that bundle and data are gone; the pending entry remains visible for retry,
even without bundle metadata. Reinstall is allowed only after cleanup completes.
After restart Settings displays the saved phase, including private-data or bundle
cleanup already completed, without constructing the plugin.
Pending-removal reads return explicit success, error and entries. A locked,
unreadable or corrupt journal is not an empty successful list. The registry keeps
its last known records, exposes the read error and refuses plugin state changes.
Settings shows that failure and a retry action, with plugin and import controls
disabled. A successful checked retry restores controls without dropping any
pending removal. Direct enable/import/grant/removal paths still check the durable
authorization and cannot bypass the disabled UI.
The desired enable IDs are part of the same checked state snapshot; registry reload
does not follow it with a second, default-on-error settings read.

Unlock keys, shared signer keys/session safety history and unrelated private trees
are not deleted. File removal is not secure erasure of backups or old disk blocks.

The example drains its ordered storage queue. The fee and fiat producers cancel
their Rust workers and retain closing instances until worker completion and join,
then acknowledge module shutdown. Their bundle format is unchanged from #69.
