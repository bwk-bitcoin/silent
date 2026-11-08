#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>

// A plugin's storage, as the calling module sees it through Host::storage(). Every
// request and every answer is one JSON object (UTF-8), so the same messages can later
// travel over an IPC connection unchanged. No pointer, native path or file descriptor
// is part of a message, and a message never says who sends it: the host binds the
// caller to the IStorage it handed out, from its own record of the plugin.
//
// Request fields (any other field, a missing one or a wrong type answers
// invalid_request):
//   id         string of 1 to MAX_ID_SIZE bytes (UTF-8), chosen by the caller, unique
//              among its outstanding requests
//   op         "discover" | "open" | "inspect" | "list" | "mkdir" | "read" | "write" |
//              "rename" | "remove" | "migrate"; any other answers invalid_request
//   storage    string: "private" for open, else the handle open answered (every op
//              but discover)
//   path       string, a logical path (below)
//   to         string, the logical path rename moves "path" to (rename)
//   data       string, base64 of the whole file (write); not base64 answers
//              invalid_request
//   mode       "create" (fails already_exists) | "replace" (fails not_found) |
//              "upsert" (write)
//   recursive  bool, remove a non-empty directory with its content (remove)
//   after      string, the "next" a previous list answered (list)
//   limit      integer from 1 to 128, 128 when absent, else invalid_request (list)
//
// Logical paths: names joined by '/', relative to the storage root. A name is 1 to 255
// bytes (UTF-8), not "." or "..", without '/', '\\', ':' or control characters; names
// are case-sensitive and never Unicode-normalized. A whole path is
// at most 4096 bytes. So no leading, trailing or doubled '/'. The empty path is the
// root: inspect and list take it, and default to it when "path" is absent; any other
// op answers invalid_path for it. A bad path, or a bad "after" name, answers
// invalid_path. An entry is created in an existing directory: a missing parent answers
// not_found, a file in place of one not_directory.
//
// Every answer carries the request's "id" and "error", empty on success, else one of:
//   locked invalid_request invalid_path invalid_handle not_found already_exists
//   not_directory is_directory not_empty too_large busy io corrupt unavailable denied
//   revoked cancelled
// An answer whose id is empty refuses a message the host could not read an id from.
//
// What each op answers on success:
//   discover  "storages": [{"id": "private", "name": "Private plugin storage"}]
//   open      "storage": an opaque handle, valid only on the IStorage that opened it;
//             another storage id answers not_found
//   inspect   "entry": {"path", "kind": "file" | "directory", "size": bytes of a file,
//             0 for a directory}
//   list      "entries": the direct entries of the directory "path", by name, after
//             "after"; "next": the "after" of the next page, empty on the last one
//   read      "data": base64 of the whole file
//   migrate   "migrated": true once the state saved by the former single-blob API was
//             written to the new file "path" and read back, false when there is no
//             such state ("reason": "absent") or migration completed before
//             ("reason": "completed"). Before completion, "path" must not exist
//             (already_exists). The legacy envelope is retained unchanged. Removing
//             "path" afterwards does not bring the old state back.
//   mkdir, write, rename, remove: nothing more
//
// Files are read and written whole: no offsets and no chunks. A write leaves either the
// previous or the new complete file, never a part of one. rename never replaces an
// existing entry (already_exists) and cannot move a directory below itself
// (invalid_path). remove of a non-empty directory needs "recursive" (not_empty). A
// request cancelled before it started changes nothing; one already running may still
// complete.
//
// Limits: a file holds at most MAX_FILE_SIZE bytes before base64 (too_large); a message
// is at most MAX_MESSAGE_SIZE bytes of JSON (too_large, with an empty id). A storage
// holds at most 4096 entries. Its whole tree, counted as one JSON document with files
// in base64, is at most 16 MiB before encryption: a change past either answers
// too_large. At most
// MAX_OUTSTANDING requests are outstanding at once, a cancelled one counted until it
// has stopped running: one more answers busy.
// Contention with another process using the same tree also answers busy without waiting.
// These are logical operations, independent of the encrypted backend and its layout.
// An embedded filesystem backend is deferred; no physical paths or handles are exposed.
//
// The modules of one plugin share its storage, each through its own IStorage, whose
// handles the others cannot use. The storage of a plugin lasts across its updates; the
// Saved Data grant of each library version is checked on every request (denied), and
// locking the app state refuses requests (locked). An answer computed before the grant
// was withdrawn or the state locked is delivered as that error, never with its data.
//
// GUI thread only. Every request gets exactly one completed() answer, on the GUI thread.
// An accepted request runs off the GUI thread and answers after request() returned.
// A request refused before it is accepted (too_large, invalid_request, busy, revoked)
// answers from inside request(), and cancel() answers the request "cancelled" from
// inside the call: connect completed() before sending. A cancelled request's later
// result is dropped. Once the host revokes the IStorage (the plugin is disabled or
// unloaded), outstanding requests get no answer and new ones answer "revoked".
// Unload and cancellation never drain workers: a commit past final authorization may
// still finish. Saved Data grant updates instead exclude final authorization through
// rename; a completed grant update fences older, not-yet-authorized publications.
//
// A plugin library runs inside Silent's process: these checks bind well-behaved
// plugins, they are not a sandbox. Over a future IPC transport, the host derives the
// caller from the authenticated connection, never from the messages.
class IStorage : public QObject {
    Q_OBJECT

public:
    static constexpr qsizetype MAX_FILE_SIZE = 1024 * 1024;
    static constexpr qsizetype MAX_MESSAGE_SIZE = 2 * 1024 * 1024;
    static constexpr int MAX_OUTSTANDING = 8;
    static constexpr qsizetype MAX_ID_SIZE = 256;

    using QObject::QObject;
    ~IStorage() override = default;

    virtual void request(const QByteArray &message) = 0;
    virtual void cancel(const QString &id) = 0;

signals:
    void completed(QByteArray message);
};
