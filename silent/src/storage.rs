//! Plugin file storage (ws-plaude/silent#60): each plugin's private tree of logical
//! files and directories, reached through bounded JSON messages on a host session.
//!
//! The host opens a session for a plugin it has authenticated (`storage_session_open`),
//! then passes the plugin's requests as JSON text (`storage_request`) and answers with
//! JSON text. The caller's identity is the session's, never a field of a request, and
//! a storage handle opened on one session means nothing on another: modules of one
//! plugin share its tree, not their handles.
//!
//! A request is a JSON object with a string `id` and an `op`; each operation accepts
//! only its own fields, any other field or operation is `invalid_request`. File data
//! is base64. The bounds: a request of `MAX_MESSAGE_BYTES` of JSON text, a file of
//! `MAX_FILE_BYTES` decoded, a tree of `MAX_ENTRIES` entries and `MAX_TREE_BYTES` of
//! JSON text before sealing, a listing page of `MAX_PAGE` entries.
//!
//! A logical path is names joined by `/`, relative to the tree's root, the root itself
//! the empty path. A name is 1 to `MAX_NAME_BYTES` bytes of UTF-8, not `.` or `..`,
//! without `/`, `\`, `:` or a control character, so no native path is ever a logical
//! one; a path is at most `MAX_PATH_BYTES` bytes. Names are compared byte for byte:
//! case-sensitive, never normalized.
//!
//! The whole tree is one document sealed with the app state key, `tree.bip138`, in
//! `<data dir>/<plugin id>/`: the plugin id is visible on disk, its names, directories
//! and bytes are not. The document names the plugin it belongs to, so a tree copied to
//! another root, or two ids sharing a root on a case-insensitive disk, is `corrupt`. A
//! change re-reads the document, applies the change and replaces the document
//! atomically, so a rename or a recursive removal lands whole or not at all. Operations
//! of every session are serialized. A persistent native file lock excludes other
//! processes from load through publication; contention is `busy`, never a wait.
//! Revoking or cancelling never waits for an operation.
//!
//! Disk access never follows a link: on Unix it is relative to directory handles
//! opened without following one (`disk` below), on Windows it refuses reparse points
//! (`storage_windows.rs`), elsewhere the storage is `unavailable`.

use std::{
    collections::{BTreeMap, HashMap, VecDeque},
    fmt,
    fs::File,
    ops::Bound,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex, MutexGuard,
    },
};

use base64::{prelude::BASE64_STANDARD, Engine};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

use crate::{
    app_state::app_plugin_grants, keys::KEYS_DIR_NAME, plugin_bundle::PLUGINS_DIR, state_crypt,
};

/// The largest file, in bytes.
pub const MAX_FILE_BYTES: usize = 1 << 20;
/// The largest request, in bytes of JSON text.
pub const MAX_MESSAGE_BYTES: usize = 2 << 20;
/// The largest serialized tree, in bytes of JSON text, before sealing.
pub const MAX_TREE_BYTES: usize = 16 << 20;
/// The most files and directories in one plugin's tree.
pub const MAX_ENTRIES: usize = 4096;
/// The most entries one `list` answers.
pub const MAX_PAGE: usize = 128;
/// The most requests a session has in flight, and the most cancellations of requests
/// not sent yet it remembers.
pub const MAX_OUTSTANDING: usize = 8;
/// The longest logical path, in bytes: bounds a full listing page under the message
/// bound.
pub const MAX_PATH_BYTES: usize = 4096;
/// The longest file or directory name, and plugin id, in bytes.
pub const MAX_NAME_BYTES: usize = 255;
/// The longest request id, in bytes.
pub const MAX_ID_BYTES: usize = 256;
/// What a BIP138 envelope adds to the text it seals: header, one key, one path.
const MAX_ENVELOPE_BYTES: usize = 64 * 1024;
/// The largest sealed tree read from disk.
const MAX_SEALED_TREE_BYTES: usize = MAX_TREE_BYTES + MAX_ENVELOPE_BYTES;
/// The largest legacy blob read from disk: the base64 of the largest file, sealed.
const MAX_SEALED_LEGACY_BYTES: usize = MAX_FILE_BYTES * 2 + MAX_ENVELOPE_BYTES;

const PRIVATE_STORAGE: &str = "private";
const PRIVATE_STORAGE_NAME: &str = "Private plugin storage";
const DOCUMENT: &str = "tree.bip138";
const DOCUMENT_TEMP: &str = "tree.tmp";
const DOCUMENT_LOCK: &str = "tree.lock";
const TREE_VERSION: u32 = 1;
const HANDLE_BYTES: usize = 16;

/// Directories the host keeps in the data directory, `locked` being where a locked
/// account path points. A plugin id never equals one, ignoring case.
const RESERVED_ROOTS: [&str; 5] = [
    KEYS_DIR_NAME,
    PLUGINS_DIR,
    state_crypt::PLUGIN_STATE_DIR,
    "musig-sessions",
    "locked",
];
/// The extensions of the host's files in the data directory (`app.bip138`,
/// `unlock.key`), their temps and lock sentinels. A plugin id never ends with one.
const RESERVED_EXTENSIONS: [&str; 4] = [state_crypt::SEALED_EXTENSION, "key", "tmp", "lock"];
/// Names Windows keeps for devices, with or without an extension.
const DEVICE_NAMES: [&str; 24] = [
    "con", "prn", "aux", "nul", "com0", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
    "com8", "com9", "lpt0", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
];
/// The length of an account directory name, the hex of an opaque name.
const ACCOUNT_DIR_LEN: usize = 32;

/// Every open session, by id. Held only to look a session up or drop it.
static SESSIONS: Mutex<BTreeMap<u64, Arc<Session>>> = Mutex::new(BTreeMap::new());
/// 0 is never a session: `storage_session_open` answers it on refusal.
static NEXT_SESSION: AtomicU64 = AtomicU64::new(1);
/// Held while a request reads or replaces a tree, by every session.
static OPERATIONS: Mutex<()> = Mutex::new(());

fn lock_file(file: &File) -> Result<(), ErrorCode> {
    file.try_lock_exclusive().map_err(|error| {
        if error.raw_os_error() == fs2::lock_contended_error().raw_os_error() {
            ErrorCode::Busy
        } else {
            error.into()
        }
    })
}

/// The typed failure a response carries in `error`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ErrorCode {
    Locked,
    Denied,
    Revoked,
    InvalidRequest,
    InvalidPath,
    InvalidHandle,
    NotFound,
    AlreadyExists,
    NotDirectory,
    IsDirectory,
    NotEmpty,
    TooLarge,
    Busy,
    Io,
    Corrupt,
    Unavailable,
    Cancelled,
}

impl ErrorCode {
    fn as_str(self) -> &'static str {
        match self {
            ErrorCode::Locked => "locked",
            ErrorCode::Denied => "denied",
            ErrorCode::Revoked => "revoked",
            ErrorCode::InvalidRequest => "invalid_request",
            ErrorCode::InvalidPath => "invalid_path",
            ErrorCode::InvalidHandle => "invalid_handle",
            ErrorCode::NotFound => "not_found",
            ErrorCode::AlreadyExists => "already_exists",
            ErrorCode::NotDirectory => "not_directory",
            ErrorCode::IsDirectory => "is_directory",
            ErrorCode::NotEmpty => "not_empty",
            ErrorCode::TooLarge => "too_large",
            ErrorCode::Busy => "busy",
            ErrorCode::Io => "io",
            ErrorCode::Corrupt => "corrupt",
            ErrorCode::Unavailable => "unavailable",
            ErrorCode::Cancelled => "cancelled",
        }
    }
}

/// A link, or another kind of file, where the storage expects a directory or the
/// document is `unavailable`; any other failure is `io`.
impl From<std::io::Error> for ErrorCode {
    fn from(error: std::io::Error) -> Self {
        log::error!("storage: {error}");
        if refused_by_disk(&error) {
            ErrorCode::Unavailable
        } else {
            ErrorCode::Io
        }
    }
}

#[cfg(unix)]
fn refused_by_disk(error: &std::io::Error) -> bool {
    use rustix::io::Errno;

    matches!(
        Errno::from_io_error(error),
        Some(Errno::LOOP | Errno::MLINK | Errno::NOTDIR | Errno::ISDIR)
    )
}

#[cfg(not(unix))]
fn refused_by_disk(_error: &std::io::Error) -> bool {
    false
}

// ===== Requests =====

/// A request, its `id` taken out: the operation names the fields it accepts.
#[derive(Debug, Deserialize)]
#[serde(tag = "op", rename_all = "lowercase", deny_unknown_fields)]
enum Operation {
    Discover {},
    Open {
        storage: String,
    },
    Inspect {
        storage: String,
        #[serde(default)]
        path: String,
    },
    List {
        storage: String,
        #[serde(default)]
        path: String,
        after: Option<String>,
        limit: Option<u64>,
    },
    Mkdir {
        storage: String,
        path: String,
    },
    Read {
        storage: String,
        path: String,
    },
    Write {
        storage: String,
        path: String,
        data: String,
        mode: WriteMode,
    },
    Rename {
        storage: String,
        path: String,
        to: String,
    },
    Remove {
        storage: String,
        path: String,
        #[serde(default)]
        recursive: bool,
    },
    Migrate {
        storage: String,
        path: String,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
enum WriteMode {
    /// Fails with `already_exists` when the file exists.
    Create,
    /// Fails with `not_found` when the file does not exist.
    Replace,
    Upsert,
}

/// A request's id, as much of it as could be read, and its operation.
fn parse(request: &str) -> Result<(String, Operation), (String, ErrorCode)> {
    if request.len() > MAX_MESSAGE_BYTES {
        return Err((String::new(), ErrorCode::TooLarge));
    }
    let Ok(Value::Object(mut fields)) = serde_json::from_str::<Value>(request) else {
        return Err((String::new(), ErrorCode::InvalidRequest));
    };
    let id = match fields.remove("id") {
        Some(Value::String(id)) => id,
        _ => return Err((String::new(), ErrorCode::InvalidRequest)),
    };
    if id.is_empty() || id.len() > MAX_ID_BYTES {
        return Err((String::new(), ErrorCode::InvalidRequest));
    }
    if fields.values().any(Value::is_null) {
        return Err((id, ErrorCode::InvalidRequest));
    }
    match serde_json::from_value(Value::Object(fields)) {
        Ok(operation) => Ok((id, operation)),
        Err(error) => {
            log::warn!("storage: request {id} refused: {error}");
            Err((id, ErrorCode::InvalidRequest))
        }
    }
}

// ===== Responses =====

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum EntryKind {
    File,
    Directory,
}

impl EntryKind {
    fn as_str(self) -> &'static str {
        match self {
            EntryKind::File => "file",
            EntryKind::Directory => "directory",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct Entry {
    path: LogicalPath,
    kind: EntryKind,
    /// The file's length in bytes, 0 for a directory.
    size: u64,
}

impl Entry {
    fn to_json(&self) -> Value {
        json!({
            "path": self.path.to_string(),
            "kind": self.kind.as_str(),
            "size": self.size,
        })
    }
}

/// How a `migrate` ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Migration {
    /// The legacy blob is now the file asked for.
    Done,
    /// The plugin never saved a legacy blob.
    Absent,
    /// An earlier migration completed: the blob is never imported again.
    Completed,
}

/// What a successful operation answers, besides its id.
#[derive(Debug)]
enum Reply {
    /// The storages the session may open: its private one.
    Storages,
    Storage(String),
    Entry(Entry),
    /// A page of a directory's entries, and the name to list after for the next page,
    /// empty on the last one.
    Listing(Vec<Entry>, String),
    Data(Vec<u8>),
    Done,
    Migrated(Migration),
}

fn response(id: &str, result: Result<Reply, ErrorCode>) -> String {
    let mut fields = Map::new();
    fields.insert("id".into(), id.into());
    let reply = match result {
        Ok(reply) => reply,
        Err(code) => {
            fields.insert("error".into(), code.as_str().into());
            return Value::Object(fields).to_string();
        }
    };
    fields.insert("error".into(), "".into());
    match reply {
        Reply::Storages => {
            let storages = json!([{"id": PRIVATE_STORAGE, "name": PRIVATE_STORAGE_NAME}]);
            fields.insert("storages".into(), storages);
        }
        Reply::Storage(handle) => {
            fields.insert("storage".into(), handle.into());
        }
        Reply::Entry(entry) => {
            fields.insert("entry".into(), entry.to_json());
        }
        Reply::Listing(entries, next) => {
            let entries: Vec<Value> = entries.iter().map(Entry::to_json).collect();
            fields.insert("entries".into(), entries.into());
            fields.insert("next".into(), next.into());
        }
        Reply::Data(data) => {
            fields.insert("data".into(), BASE64_STANDARD.encode(data).into());
        }
        Reply::Done => {}
        Reply::Migrated(Migration::Done) => {
            fields.insert("migrated".into(), true.into());
        }
        Reply::Migrated(Migration::Absent) => {
            fields.insert("migrated".into(), false.into());
            fields.insert("reason".into(), "absent".into());
        }
        Reply::Migrated(Migration::Completed) => {
            fields.insert("migrated".into(), false.into());
            fields.insert("reason".into(), "completed".into());
        }
    }
    Value::Object(fields).to_string()
}

// ===== Logical paths =====

/// A path in a plugin's tree: names separated by `/`, relative to its root, the root
/// itself empty. Ordered name by name, so a directory's descendants follow it.
#[derive(Debug, Clone, Default, PartialEq, Eq, PartialOrd, Ord)]
struct LogicalPath(Vec<String>);

impl LogicalPath {
    fn parse(text: &str) -> Result<Self, ErrorCode> {
        if text.is_empty() {
            return Ok(Self::default());
        }
        if text.len() > MAX_PATH_BYTES {
            return Err(ErrorCode::InvalidPath);
        }
        text.split('/')
            .map(|name| {
                valid_name(name)
                    .then(|| name.to_string())
                    .ok_or(ErrorCode::InvalidPath)
            })
            .collect::<Result<_, _>>()
            .map(Self)
    }

    /// Parses a path that names an entry, never the root.
    fn parse_entry(text: &str) -> Result<Self, ErrorCode> {
        let path = Self::parse(text)?;
        if path.is_root() {
            return Err(ErrorCode::InvalidPath);
        }
        Ok(path)
    }

    fn is_root(&self) -> bool {
        self.0.is_empty()
    }

    fn child(&self, name: &str) -> Self {
        let mut names = self.0.clone();
        names.push(name.to_string());
        Self(names)
    }

    fn name(&self) -> &str {
        self.0.last().map(String::as_str).unwrap_or_default()
    }

    fn depth(&self) -> usize {
        self.0.len()
    }

    /// The length of the path's text, in bytes.
    fn byte_len(&self) -> usize {
        self.0.iter().map(String::len).sum::<usize>() + self.0.len().saturating_sub(1)
    }

    /// Whether `self` is `ancestor` or below it.
    fn starts_with(&self, ancestor: &LogicalPath) -> bool {
        self.0.starts_with(&ancestor.0)
    }

    /// Every directory above `self`, the root excluded, the top one first.
    fn ancestors(&self) -> impl Iterator<Item = LogicalPath> + '_ {
        (1..self.0.len()).map(|len| Self(self.0[..len].to_vec()))
    }

    /// `self`, below `from`, moved below `to`.
    fn moved(&self, from: &LogicalPath, to: &LogicalPath) -> Self {
        let mut names = to.0.clone();
        names.extend_from_slice(&self.0[from.0.len()..]);
        Self(names)
    }
}

impl fmt::Display for LogicalPath {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0.join("/"))
    }
}

fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= MAX_NAME_BYTES
        && name != "."
        && name != ".."
        && !name.contains(['/', '\\', ':'])
        && !name.chars().any(char::is_control)
}

/// Whether `plugin_id` may name a root in the data directory: never one of the host's
/// entries, an account directory or a name Windows keeps or trims, ignoring case.
fn valid_root(plugin_id: &str) -> bool {
    let stem = plugin_id.split('.').next().unwrap_or_default();
    let extension = plugin_id.rsplit_once('.').map(|(_, extension)| extension);
    let account_dir =
        plugin_id.len() == ACCOUNT_DIR_LEN && plugin_id.bytes().all(|b| b.is_ascii_hexdigit());
    state_crypt::valid_plugin_id(plugin_id)
        && plugin_id.len() <= MAX_NAME_BYTES
        && !plugin_id.ends_with('.')
        && !account_dir
        && !RESERVED_ROOTS
            .iter()
            .any(|reserved| reserved.eq_ignore_ascii_case(plugin_id))
        && !extension.is_some_and(|extension| {
            RESERVED_EXTENSIONS
                .iter()
                .any(|reserved| reserved.eq_ignore_ascii_case(extension))
        })
        && !DEVICE_NAMES
            .iter()
            .any(|device| device.eq_ignore_ascii_case(stem))
}

// ===== Tree =====

#[derive(Debug, Clone, PartialEq, Eq)]
enum Node {
    Directory,
    File(Vec<u8>),
}

static ROOT: Node = Node::Directory;

/// The tree as sealed on disk: the plugin it belongs to, whether its legacy blob was
/// migrated, and every entry with its full path, parents first.
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Document {
    version: u32,
    owner: String,
    migrated: bool,
    entries: Vec<DocumentEntry>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase", deny_unknown_fields)]
enum DocumentEntry {
    Directory { path: String },
    File { path: String, data: String },
}

/// A plugin's tree, every entry by its path, the root implied.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct Tree {
    nodes: BTreeMap<LogicalPath, Node>,
    /// The legacy blob was migrated: it is never imported again, even once the file it
    /// became is gone.
    migrated: bool,
}

impl Tree {
    /// Reads the document of `owner`, refusing anything this module would not have
    /// written for it.
    fn decode(text: &str, owner: &str) -> Result<Self, ErrorCode> {
        let document: Document = serde_json::from_str(text).map_err(|error| {
            log::error!("storage: unreadable tree of '{owner}': {error}");
            ErrorCode::Corrupt
        })?;
        if document.version != TREE_VERSION || document.entries.len() > MAX_ENTRIES {
            return Err(ErrorCode::Corrupt);
        }
        if document.owner != owner {
            log::error!(
                "storage: the tree in the root of '{owner}' belongs to '{}'",
                document.owner
            );
            return Err(ErrorCode::Corrupt);
        }
        let mut tree = Self {
            nodes: BTreeMap::new(),
            migrated: document.migrated,
        };
        for entry in document.entries {
            let (path, node) = match entry {
                DocumentEntry::Directory { path } => (path, Node::Directory),
                DocumentEntry::File { path, data } => {
                    let data = BASE64_STANDARD
                        .decode(data)
                        .map_err(|_| ErrorCode::Corrupt)?;
                    if data.len() > MAX_FILE_BYTES {
                        return Err(ErrorCode::Corrupt);
                    }
                    (path, Node::File(data))
                }
            };
            let path = LogicalPath::parse_entry(&path).map_err(|_| ErrorCode::Corrupt)?;
            if tree.nodes.insert(path, node).is_some() {
                return Err(ErrorCode::Corrupt);
            }
        }
        let orphan = tree.nodes.keys().any(|path| {
            path.ancestors()
                .any(|ancestor| tree.nodes.get(&ancestor) != Some(&Node::Directory))
        });
        if orphan {
            return Err(ErrorCode::Corrupt);
        }
        Ok(tree)
    }

    /// The document of `owner` holding this tree.
    fn encode(&self, owner: &str) -> Result<String, ErrorCode> {
        let entries = self
            .nodes
            .iter()
            .map(|(path, node)| match node {
                Node::Directory => DocumentEntry::Directory {
                    path: path.to_string(),
                },
                Node::File(data) => DocumentEntry::File {
                    path: path.to_string(),
                    data: BASE64_STANDARD.encode(data),
                },
            })
            .collect();
        let document = Document {
            version: TREE_VERSION,
            owner: owner.to_string(),
            migrated: self.migrated,
            entries,
        };
        let text = serde_json::to_string(&document).map_err(|error| {
            log::error!("storage: cannot serialize the tree of '{owner}': {error}");
            ErrorCode::Io
        })?;
        if text.len() > MAX_TREE_BYTES {
            return Err(ErrorCode::TooLarge);
        }
        Ok(text)
    }

    /// The node at `path`, `None` when its parent holds nothing by that name.
    fn find(&self, path: &LogicalPath) -> Result<Option<&Node>, ErrorCode> {
        if path.is_root() {
            return Ok(Some(&ROOT));
        }
        for ancestor in path.ancestors() {
            match self.nodes.get(&ancestor) {
                None => return Err(ErrorCode::NotFound),
                Some(Node::File(_)) => return Err(ErrorCode::NotDirectory),
                Some(Node::Directory) => {}
            }
        }
        Ok(self.nodes.get(path))
    }

    fn existing(&self, path: &LogicalPath) -> Result<&Node, ErrorCode> {
        self.find(path)?.ok_or(ErrorCode::NotFound)
    }

    fn entry(&self, path: &LogicalPath) -> Result<Entry, ErrorCode> {
        let (kind, size) = match self.existing(path)? {
            Node::Directory => (EntryKind::Directory, 0),
            Node::File(data) => (EntryKind::File, data.len() as u64),
        };
        Ok(Entry {
            path: path.clone(),
            kind,
            size,
        })
    }

    /// Up to `limit` entries of the directory `path`, by name, after the name `after`,
    /// and the name to list after next, empty when none is left.
    fn list(
        &self,
        path: &LogicalPath,
        after: Option<&str>,
        limit: usize,
    ) -> Result<(Vec<Entry>, String), ErrorCode> {
        if self.existing(path)? != &Node::Directory {
            return Err(ErrorCode::NotDirectory);
        }
        let start = after.map_or_else(|| path.clone(), |name| path.child(name));
        let mut children = self
            .nodes
            .range((Bound::Excluded(start), Bound::Unbounded))
            .take_while(|(child, _)| child.starts_with(path))
            .filter(|(child, _)| child.depth() == path.depth() + 1)
            .map(|(child, _)| child.clone());
        let mut entries = Vec::new();
        for child in children.by_ref().take(limit) {
            entries.push(self.entry(&child)?);
        }
        let next = match (children.next(), entries.last()) {
            (Some(_), Some(last)) => last.path.name().to_string(),
            _ => String::new(),
        };
        Ok((entries, next))
    }

    fn read(&self, path: &LogicalPath) -> Result<Vec<u8>, ErrorCode> {
        match self.existing(path)? {
            Node::Directory => Err(ErrorCode::IsDirectory),
            Node::File(data) => Ok(data.clone()),
        }
    }

    /// Adds `node` at `path`, its parent an existing directory, nothing there yet.
    fn create(&mut self, path: &LogicalPath, node: Node) -> Result<(), ErrorCode> {
        if self.find(path)?.is_some() {
            return Err(ErrorCode::AlreadyExists);
        }
        if self.nodes.len() >= MAX_ENTRIES {
            return Err(ErrorCode::TooLarge);
        }
        self.nodes.insert(path.clone(), node);
        Ok(())
    }

    fn write(
        &mut self,
        path: &LogicalPath,
        data: Vec<u8>,
        mode: WriteMode,
    ) -> Result<(), ErrorCode> {
        match (self.find(path)?, mode) {
            (Some(Node::Directory), _) => Err(ErrorCode::IsDirectory),
            (Some(Node::File(_)), WriteMode::Create) => Err(ErrorCode::AlreadyExists),
            (None, WriteMode::Replace) => Err(ErrorCode::NotFound),
            (Some(Node::File(_)), WriteMode::Replace | WriteMode::Upsert) => {
                self.nodes.insert(path.clone(), Node::File(data));
                Ok(())
            }
            (None, WriteMode::Create | WriteMode::Upsert) => self.create(path, Node::File(data)),
        }
    }

    /// `path` and every entry below it.
    fn subtree(&self, path: &LogicalPath) -> Vec<LogicalPath> {
        self.nodes
            .range(path..)
            .take_while(|(entry, _)| entry.starts_with(path))
            .map(|(entry, _)| entry.clone())
            .collect()
    }

    /// Moves `from` and everything below it to `to`, which must not exist yet. Every
    /// moved path is checked before anything moves.
    fn rename(&mut self, from: &LogicalPath, to: &LogicalPath) -> Result<(), ErrorCode> {
        self.existing(from)?;
        if to.starts_with(from) {
            return Err(ErrorCode::InvalidPath);
        }
        if self.find(to)?.is_some() {
            return Err(ErrorCode::AlreadyExists);
        }
        let moves: Vec<(LogicalPath, LogicalPath)> = self
            .subtree(from)
            .into_iter()
            .map(|path| {
                let moved = path.moved(from, to);
                (path, moved)
            })
            .collect();
        if moves
            .iter()
            .any(|(_, moved)| moved.byte_len() > MAX_PATH_BYTES)
        {
            return Err(ErrorCode::InvalidPath);
        }
        for (path, moved) in moves {
            if let Some(node) = self.nodes.remove(&path) {
                self.nodes.insert(moved, node);
            }
        }
        Ok(())
    }

    fn remove(&mut self, path: &LogicalPath, recursive: bool) -> Result<(), ErrorCode> {
        self.existing(path)?;
        let subtree = self.subtree(path);
        if subtree.len() > 1 && !recursive {
            return Err(ErrorCode::NotEmpty);
        }
        for entry in subtree {
            self.nodes.remove(&entry);
        }
        Ok(())
    }
}

// ===== Sessions =====

/// What the host authenticated a caller as, and what it has outstanding.
#[derive(Debug)]
struct Session {
    data_dir: PathBuf,
    plugin_id: String,
    /// The plugin library's sha256, whose Saved Data grant the session needs.
    plugin_hash: String,
    /// False for a module the host itself ships: it needs no grant.
    external: bool,
    /// The only storage handle valid on this session.
    handle: String,
    revoked: AtomicBool,
    /// Bumped when the session's grant or the unlock key changes: an answer computed
    /// before is not delivered (`storage_delivery_error`).
    generation: AtomicU64,
    requests: Mutex<Requests>,
}

#[derive(Debug, Default)]
struct Requests {
    /// Each request in flight, by id, and whether it was cancelled.
    running: HashMap<String, Arc<AtomicBool>>,
    /// The last ids cancelled before they were sent, so they answer `cancelled` when
    /// they come. A cancellation racing the answer leaves a stale id: bounded, the
    /// oldest goes first, and the host forgets it (`storage_request_forget`).
    cancelled: VecDeque<String>,
}

impl Session {
    fn new(
        data_dir: PathBuf,
        plugin_id: String,
        plugin_hash: String,
        external: bool,
    ) -> Result<Self, String> {
        if !valid_root(&plugin_id) {
            return Err(format!("invalid plugin id '{plugin_id}'"));
        }
        if external && plugin_hash.is_empty() {
            return Err(format!(
                "no library hash for the external plugin '{plugin_id}'"
            ));
        }
        let mut handle = [0u8; HANDLE_BYTES];
        getrandom::getrandom(&mut handle).map_err(|error| error.to_string())?;
        Ok(Self {
            data_dir,
            plugin_id,
            plugin_hash,
            external,
            handle: hex::encode(handle),
            revoked: AtomicBool::new(false),
            generation: AtomicU64::new(0),
            requests: Mutex::new(Requests::default()),
        })
    }

    fn requests(&self) -> MutexGuard<'_, Requests> {
        self.requests.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn revoke(&self) {
        self.revoked.store(true, Ordering::SeqCst);
        for cancelled in self.requests().running.values() {
            cancelled.store(true, Ordering::SeqCst);
        }
    }

    fn cancel(&self, id: String) {
        let mut requests = self.requests();
        if let Some(cancelled) = requests.running.get(&id) {
            cancelled.store(true, Ordering::SeqCst);
            return;
        }
        if requests.cancelled.contains(&id) {
            return;
        }
        if requests.cancelled.len() == MAX_OUTSTANDING {
            requests.cancelled.pop_front();
        }
        requests.cancelled.push_back(id);
    }

    fn forget(&self, id: &str) {
        self.requests()
            .cancelled
            .retain(|cancelled| cancelled != id);
    }

    fn granted(&self) -> bool {
        !self.external || app_plugin_grants(self.plugin_hash.clone()).state
    }

    fn check_handle(&self, storage: &str) -> Result<(), ErrorCode> {
        if storage != self.handle {
            return Err(ErrorCode::InvalidHandle);
        }
        Ok(())
    }

    /// Why an answer computed at `generation` must not be delivered, read from memory
    /// only.
    fn delivery_error(&self, generation: u64) -> Option<ErrorCode> {
        if self.revoked.load(Ordering::SeqCst) {
            Some(ErrorCode::Revoked)
        } else if !state_crypt::holds_key() {
            Some(ErrorCode::Locked)
        } else if self.generation.load(Ordering::SeqCst) != generation {
            Some(ErrorCode::Denied)
        } else {
            None
        }
    }
}

fn lookup(session: u64) -> Option<Arc<Session>> {
    SESSIONS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get(&session)
        .cloned()
}

fn open_session(data_dir: PathBuf, plugin_id: String, plugin_hash: String, external: bool) -> u64 {
    match Session::new(data_dir, plugin_id, plugin_hash, external) {
        Ok(session) => {
            let id = NEXT_SESSION.fetch_add(1, Ordering::SeqCst);
            SESSIONS
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .insert(id, Arc::new(session));
            id
        }
        Err(error) => {
            log::error!("storage: no session: {error}");
            0
        }
    }
}

/// Bumps the generation of every session `select` picks.
fn bump_generations(select: impl Fn(&Session) -> bool) {
    for session in SESSIONS.lock().unwrap_or_else(|e| e.into_inner()).values() {
        if select(session) {
            session.generation.fetch_add(1, Ordering::SeqCst);
        }
    }
}

/// The Saved Data grant of the library `hash` changed: answers its sessions computed
/// before are not delivered. The caller holds the exclusive publication guard.
pub(crate) fn grants_changed(hash: &str) {
    let hash = hash.to_ascii_lowercase();
    bump_generations(|session| session.external && session.plugin_hash == hash);
}

/// The unlock key was set or dropped: no answer computed before is delivered.
pub(crate) fn key_changed() {
    bump_generations(|_| true);
}

/// A request in flight on its session, forgotten when dropped.
struct Pending {
    session: Arc<Session>,
    id: String,
    cancelled: Arc<AtomicBool>,
    generation: u64,
}

impl Pending {
    fn register(session: Arc<Session>, id: String) -> Result<Self, ErrorCode> {
        let generation = session.generation.load(Ordering::SeqCst);
        let cancelled = {
            let mut requests = session.requests();
            if session.revoked.load(Ordering::SeqCst) {
                return Err(ErrorCode::Revoked);
            }
            if let Some(index) = requests
                .cancelled
                .iter()
                .position(|cancelled| *cancelled == id)
            {
                requests.cancelled.remove(index);
                return Err(ErrorCode::Cancelled);
            }
            if requests.running.contains_key(&id) {
                return Err(ErrorCode::InvalidRequest);
            }
            if requests.running.len() >= MAX_OUTSTANDING {
                return Err(ErrorCode::Busy);
            }
            let cancelled = Arc::new(AtomicBool::new(false));
            requests.running.insert(id.clone(), cancelled.clone());
            cancelled
        };
        Ok(Self {
            session,
            id,
            cancelled,
            generation,
        })
    }

    /// Whether the request may still act or answer: the session open, the request
    /// not cancelled, the state unlocked, the grant still given. The grant is read from
    /// the sealed app state, so a locked state is `locked`, not `denied`.
    fn authorize(&self) -> Result<(), ErrorCode> {
        self.check_lifecycle()?;
        if !state_crypt::holds_key() {
            return Err(ErrorCode::Locked);
        }
        if !self.session.granted() {
            return Err(ErrorCode::Denied);
        }
        if self.session.generation.load(Ordering::SeqCst) != self.generation {
            return Err(ErrorCode::Denied);
        }
        // Unload/cancellation may have completed while the grant was read from disk.
        self.check_lifecycle()
    }

    fn check_lifecycle(&self) -> Result<(), ErrorCode> {
        if self.session.revoked.load(Ordering::SeqCst) {
            return Err(ErrorCode::Revoked);
        }
        if self.cancelled.load(Ordering::SeqCst) {
            return Err(ErrorCode::Cancelled);
        }
        Ok(())
    }
}

impl Drop for Pending {
    fn drop(&mut self) {
        self.session.requests().running.remove(&self.id);
    }
}

// ===== Operations =====

/// A request's hold on its plugin's tree: every session's requests take turns.
struct Store<'a> {
    pending: &'a Pending,
    root: Option<disk::LockedRoot>,
    _turn: MutexGuard<'static, ()>,
}

impl<'a> Store<'a> {
    /// Waits for the request's turn, its handle checked first.
    fn open(pending: &'a Pending, storage: &str) -> Result<Self, ErrorCode> {
        pending.session.check_handle(storage)?;
        Self::lock(pending, false)
    }

    fn open_mut(pending: &'a Pending, storage: &str) -> Result<Self, ErrorCode> {
        pending.session.check_handle(storage)?;
        Self::lock(pending, true)
    }

    fn lock(pending: &'a Pending, create: bool) -> Result<Self, ErrorCode> {
        let turn = OPERATIONS.lock().unwrap_or_else(|e| e.into_inner());
        // Revoked, cancelled or denied while waiting for its turn.
        pending.authorize()?;
        let root = disk::LockedRoot::open(
            &pending.session.data_dir,
            &pending.session.plugin_id,
            create,
        )?;
        Ok(Self {
            pending,
            root,
            _turn: turn,
        })
    }

    fn session(&self) -> &Session {
        &self.pending.session
    }

    /// The tree on disk now, empty when the plugin never stored anything.
    fn load(&self) -> Result<Tree, ErrorCode> {
        let session = self.session();
        let Some(root) = &self.root else {
            return Ok(Tree::default());
        };
        let Some(sealed) = root.read()? else {
            return Ok(Tree::default());
        };
        if !state_crypt::is_sealed(&sealed) {
            log::error!(
                "storage: the tree of '{}' is not encrypted",
                session.plugin_id
            );
            return Err(ErrorCode::Corrupt);
        }
        let text = state_crypt::open(&sealed).map_err(|error| {
            log::error!(
                "storage: cannot open the tree of '{}': {error}",
                session.plugin_id
            );
            locked_or(ErrorCode::Corrupt)
        })?;
        Tree::decode(&text, &session.plugin_id)
    }

    /// Replaces the tree on disk with `tree`. The request is authorized again once the
    /// new tree is written and synced, right before it replaces the old one.
    fn publish(&self, tree: &Tree) -> Result<(), ErrorCode> {
        let session = self.session();
        let text = tree.encode(&session.plugin_id)?;
        let sealed = state_crypt::seal(&text).map_err(|error| {
            log::error!(
                "storage: cannot seal the tree of '{}': {error}",
                session.plugin_id
            );
            locked_or(ErrorCode::Io)
        })?;
        self.root
            .as_ref()
            .ok_or(ErrorCode::Io)?
            .replace(&sealed, || {
                let guard = state_crypt::publication_guard();
                self.pending.authorize()?;
                Ok(guard)
            })
    }

    /// The bytes of the blob the plugin saved before its file storage, `None` when it
    /// never saved one.
    fn read_legacy(&self) -> Result<Option<Vec<u8>>, ErrorCode> {
        let session = self.session();
        let name = state_crypt::legacy_plugin_state_name(&session.plugin_id).map_err(|error| {
            log::error!("storage: no legacy state name: {error}");
            locked_or(ErrorCode::Io)
        })?;
        let Some(sealed) =
            disk::read_legacy(&session.data_dir, &name, MAX_SEALED_LEGACY_BYTES as u64)?
        else {
            return Ok(None);
        };
        if !state_crypt::is_sealed(&sealed) {
            log::error!(
                "storage: the legacy state of '{}' is not encrypted",
                session.plugin_id
            );
            return Err(ErrorCode::Corrupt);
        }
        let text = state_crypt::open(&sealed).map_err(|error| {
            log::error!(
                "storage: cannot open the legacy state of '{}': {error}",
                session.plugin_id
            );
            locked_or(ErrorCode::Corrupt)
        })?;
        let data = BASE64_STANDARD
            .decode(text.trim())
            .map_err(|_| ErrorCode::Corrupt)?;
        if data.len() > MAX_FILE_BYTES {
            return Err(ErrorCode::TooLarge);
        }
        Ok(Some(data))
    }
}

/// `locked` when the key went away, else `code`.
fn locked_or(code: ErrorCode) -> ErrorCode {
    if state_crypt::holds_key() {
        code
    } else {
        ErrorCode::Locked
    }
}

fn execute(pending: &Pending, operation: Operation) -> Result<Reply, ErrorCode> {
    pending.authorize()?;
    match operation {
        Operation::Discover {} => Ok(Reply::Storages),
        Operation::Open { storage } => open(pending, &storage),
        Operation::Inspect { storage, path } => inspect(pending, &storage, &path),
        Operation::List {
            storage,
            path,
            after,
            limit,
        } => list(pending, &storage, &path, after.as_deref(), limit),
        Operation::Mkdir { storage, path } => mkdir(pending, &storage, &path),
        Operation::Read { storage, path } => read(pending, &storage, &path),
        Operation::Write {
            storage,
            path,
            data,
            mode,
        } => write(pending, &storage, &path, &data, mode),
        Operation::Rename { storage, path, to } => rename(pending, &storage, &path, &to),
        Operation::Remove {
            storage,
            path,
            recursive,
        } => remove(pending, &storage, &path, recursive),
        Operation::Migrate { storage, path } => migrate(pending, &storage, &path),
    }
}

/// Answers the session's handle once its tree is readable: a root that is a link, or
/// a tree that is corrupt or another plugin's, fails here.
fn open(pending: &Pending, storage: &str) -> Result<Reply, ErrorCode> {
    if storage != PRIVATE_STORAGE {
        return Err(ErrorCode::NotFound);
    }
    let store = Store::lock(pending, false)?;
    store.load()?;
    Ok(Reply::Storage(pending.session.handle.clone()))
}

fn inspect(pending: &Pending, storage: &str, path: &str) -> Result<Reply, ErrorCode> {
    let path = LogicalPath::parse(path)?;
    let store = Store::open(pending, storage)?;
    store.load()?.entry(&path).map(Reply::Entry)
}

fn list(
    pending: &Pending,
    storage: &str,
    path: &str,
    after: Option<&str>,
    limit: Option<u64>,
) -> Result<Reply, ErrorCode> {
    let path = LogicalPath::parse(path)?;
    if after.is_some_and(|name| !valid_name(name)) {
        return Err(ErrorCode::InvalidPath);
    }
    let limit = match limit {
        None => MAX_PAGE,
        Some(limit) if (1..=MAX_PAGE as u64).contains(&limit) => limit as usize,
        Some(_) => return Err(ErrorCode::InvalidRequest),
    };
    let store = Store::open(pending, storage)?;
    let (entries, next) = store.load()?.list(&path, after, limit)?;
    Ok(Reply::Listing(entries, next))
}

fn mkdir(pending: &Pending, storage: &str, path: &str) -> Result<Reply, ErrorCode> {
    let path = LogicalPath::parse_entry(path)?;
    let store = Store::open_mut(pending, storage)?;
    let mut tree = store.load()?;
    tree.create(&path, Node::Directory)?;
    store.publish(&tree)?;
    Ok(Reply::Done)
}

fn read(pending: &Pending, storage: &str, path: &str) -> Result<Reply, ErrorCode> {
    let path = LogicalPath::parse_entry(path)?;
    let store = Store::open(pending, storage)?;
    store.load()?.read(&path).map(Reply::Data)
}

fn write(
    pending: &Pending,
    storage: &str,
    path: &str,
    data: &str,
    mode: WriteMode,
) -> Result<Reply, ErrorCode> {
    let path = LogicalPath::parse_entry(path)?;
    let data = BASE64_STANDARD
        .decode(data)
        .map_err(|_| ErrorCode::InvalidRequest)?;
    if data.len() > MAX_FILE_BYTES {
        return Err(ErrorCode::TooLarge);
    }
    let store = Store::open_mut(pending, storage)?;
    let mut tree = store.load()?;
    tree.write(&path, data, mode)?;
    store.publish(&tree)?;
    Ok(Reply::Done)
}

fn rename(pending: &Pending, storage: &str, path: &str, to: &str) -> Result<Reply, ErrorCode> {
    let from = LogicalPath::parse_entry(path)?;
    let to = LogicalPath::parse_entry(to)?;
    let store = Store::open_mut(pending, storage)?;
    let mut tree = store.load()?;
    tree.rename(&from, &to)?;
    store.publish(&tree)?;
    Ok(Reply::Done)
}

fn remove(
    pending: &Pending,
    storage: &str,
    path: &str,
    recursive: bool,
) -> Result<Reply, ErrorCode> {
    let path = LogicalPath::parse_entry(path)?;
    let store = Store::open_mut(pending, storage)?;
    let mut tree = store.load()?;
    tree.remove(&path, recursive)?;
    store.publish(&tree)?;
    Ok(Reply::Done)
}

/// Copies the blob the plugin saved before its file storage to the file `path`, never
/// replacing one, and marks the tree migrated in the same replacement, so the blob is
/// imported once. The blob itself is kept, sealed as it was.
fn migrate(pending: &Pending, storage: &str, path: &str) -> Result<Reply, ErrorCode> {
    let path = LogicalPath::parse_entry(path)?;
    let store = Store::open_mut(pending, storage)?;
    let mut tree = store.load()?;
    if tree.migrated {
        return Ok(Reply::Migrated(Migration::Completed));
    }
    let Some(data) = store.read_legacy()? else {
        return Ok(Reply::Migrated(Migration::Absent));
    };
    tree.write(&path, data.clone(), WriteMode::Create)?;
    tree.migrated = true;
    store.publish(&tree)?;
    let stored = store.load()?;
    if !stored.migrated || stored.read(&path)? != data {
        log::error!(
            "storage: '{}' migrated, but reads back different",
            store.session().plugin_id
        );
        return Err(ErrorCode::Corrupt);
    }
    Ok(Reply::Migrated(Migration::Done))
}

// ===== Disk =====
//
// Each platform pins the root and holds its native exclusive lock in `LockedRoot`
// from load through publication/readback, without following links or reparse points.
// - `read()`: the sealed tree, `None` when absent, `corrupt` past the size bound;
// - `replace(sealed, authorize)`: writes and syncs a new private temp, retains the
//   authorization guard through rename and syncs the directory;
// - `read_legacy(data_dir, name, limit)`: the file `name` in the legacy plugin state
//   directory, `None` when there is none, `too_large` past `limit` bytes.

#[cfg(unix)]
mod disk {
    use std::{
        ffi::OsStr,
        fs::File,
        io::Read,
        os::fd::{AsFd, OwnedFd},
        path::Path,
    };

    use rustix::{
        fs::{fstat, fsync, mkdirat, open, openat, FileType, Mode, OFlags},
        io::Errno,
    };

    use crate::{
        state_crypt,
        storage::{
            lock_file, ErrorCode, DOCUMENT, DOCUMENT_LOCK, DOCUMENT_TEMP, MAX_SEALED_TREE_BYTES,
        },
    };

    const DIRECTORY: OFlags = OFlags::RDONLY
        .union(OFlags::DIRECTORY)
        .union(OFlags::NOFOLLOW)
        .union(OFlags::CLOEXEC);

    pub(crate) struct LockedRoot {
        root: OwnedFd,
        _lock: File,
    }

    impl LockedRoot {
        pub(crate) fn open(
            data_dir: &Path,
            plugin_id: &str,
            create: bool,
        ) -> Result<Option<Self>, ErrorCode> {
            let Some(root) = root(data_dir, plugin_id, create)? else {
                return Ok(None);
            };
            let lock = openat(
                &root,
                DOCUMENT_LOCK,
                OFlags::RDWR
                    | OFlags::CREATE
                    | OFlags::NOFOLLOW
                    | OFlags::CLOEXEC
                    | OFlags::NONBLOCK,
                Mode::RUSR | Mode::WUSR,
            )
            .map_err(std::io::Error::from)?;
            let stat = fstat(&lock).map_err(std::io::Error::from)?;
            if FileType::from_raw_mode(stat.st_mode) != FileType::RegularFile || stat.st_nlink != 1
            {
                return Err(ErrorCode::Unavailable);
            }
            let lock = File::from(lock);
            lock_file(&lock)?;
            Ok(Some(Self { root, _lock: lock }))
        }

        pub(crate) fn read(&self) -> Result<Option<Vec<u8>>, ErrorCode> {
            match read_file(&self.root, DOCUMENT, MAX_SEALED_TREE_BYTES as u64) {
                Err(ErrorCode::TooLarge) => Err(ErrorCode::Corrupt),
                read => read,
            }
        }

        pub(crate) fn replace<G>(
            &self,
            sealed: &[u8],
            authorize: impl FnOnce() -> Result<G, ErrorCode>,
        ) -> Result<(), ErrorCode> {
            state_crypt::replace_at(
                self.root.as_fd(),
                OsStr::new(DOCUMENT),
                OsStr::new(DOCUMENT_TEMP),
                sealed,
                authorize,
            )
        }
    }

    #[cfg(test)]
    pub(crate) fn replace(
        data_dir: &Path,
        plugin_id: &str,
        sealed: &[u8],
        authorize: impl FnOnce() -> Result<(), ErrorCode>,
    ) -> Result<(), ErrorCode> {
        LockedRoot::open(data_dir, plugin_id, true)?
            .ok_or(ErrorCode::Io)?
            .replace(sealed, authorize)
    }

    pub(crate) fn read_legacy(
        data_dir: &Path,
        name: &str,
        limit: u64,
    ) -> Result<Option<Vec<u8>>, ErrorCode> {
        let Some(data) = data_dir_handle(data_dir, false)? else {
            return Ok(None);
        };
        let Some(legacy) = directory_at(&data, state_crypt::PLUGIN_STATE_DIR)? else {
            return Ok(None);
        };
        read_file(&legacy, name, limit)
    }

    /// The configured data directory, `None` when absent and not `create`. It is the
    /// host's own choice: it may be a link.
    fn data_dir_handle(data_dir: &Path, create: bool) -> Result<Option<OwnedFd>, ErrorCode> {
        let flags = OFlags::RDONLY | OFlags::DIRECTORY | OFlags::CLOEXEC;
        match open(data_dir, flags, Mode::empty()) {
            Ok(data) => Ok(Some(data)),
            Err(Errno::NOENT) if !create => Ok(None),
            Err(error) => Err(std::io::Error::from(error).into()),
        }
    }

    /// The directory `name` in `parent`, never through a link, `None` when absent.
    fn directory_at(parent: &OwnedFd, name: &str) -> Result<Option<OwnedFd>, ErrorCode> {
        match openat(parent, name, DIRECTORY, Mode::empty()) {
            Ok(directory) => Ok(Some(directory)),
            Err(Errno::NOENT) => Ok(None),
            Err(error) => Err(std::io::Error::from(error).into()),
        }
    }

    /// The directory of `plugin_id` in the data directory: created when `create`, else
    /// `None` when absent.
    fn root(data_dir: &Path, plugin_id: &str, create: bool) -> Result<Option<OwnedFd>, ErrorCode> {
        let Some(data) = data_dir_handle(data_dir, create)? else {
            return Ok(None);
        };
        if let Some(root) = directory_at(&data, plugin_id)? {
            if create {
                // Also persist a root left by an interrupted creation.
                fsync(&data).map_err(std::io::Error::from)?;
            }
            return Ok(Some(root));
        }
        if !create {
            return Ok(None);
        }
        match mkdirat(&data, plugin_id, Mode::RWXU) {
            Ok(()) => fsync(&data).map_err(std::io::Error::from)?,
            Err(Errno::EXIST) => {}
            Err(error) => return Err(std::io::Error::from(error).into()),
        }
        directory_at(&data, plugin_id)?
            .ok_or(ErrorCode::Io)
            .map(Some)
    }

    /// The regular file `name` in `directory`, never through a link, `None` when absent,
    /// `too_large` past `limit`. Opened non-blocking, so a FIFO in its place cannot
    /// hang the open.
    fn read_file(
        directory: &OwnedFd,
        name: &str,
        limit: u64,
    ) -> Result<Option<Vec<u8>>, ErrorCode> {
        let flags = OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK;
        let file = match openat(directory, name, flags, Mode::empty()) {
            Ok(file) => file,
            Err(Errno::NOENT) => return Ok(None),
            Err(error) => return Err(std::io::Error::from(error).into()),
        };
        let stat = fstat(&file).map_err(std::io::Error::from)?;
        if FileType::from_raw_mode(stat.st_mode) != FileType::RegularFile {
            log::error!("storage: {name} is not a regular file");
            return Err(ErrorCode::Unavailable);
        }
        let mut bytes = Vec::new();
        File::from(file).take(limit + 1).read_to_end(&mut bytes)?;
        if bytes.len() as u64 > limit {
            return Err(ErrorCode::TooLarge);
        }
        Ok(Some(bytes))
    }
}

#[cfg(windows)]
#[path = "storage_windows.rs"]
mod disk;

#[cfg(not(any(unix, windows)))]
mod disk {
    use std::path::Path;

    use crate::storage::ErrorCode;

    pub(crate) struct LockedRoot;

    impl LockedRoot {
        pub(crate) fn open(
            _data_dir: &Path,
            _plugin_id: &str,
            _create: bool,
        ) -> Result<Option<Self>, ErrorCode> {
            Err(ErrorCode::Unavailable)
        }

        pub(crate) fn read(&self) -> Result<Option<Vec<u8>>, ErrorCode> {
            Err(ErrorCode::Unavailable)
        }

        pub(crate) fn replace<G>(
            &self,
            _sealed: &[u8],
            _authorize: impl FnOnce() -> Result<G, ErrorCode>,
        ) -> Result<(), ErrorCode> {
            Err(ErrorCode::Unavailable)
        }
    }

    pub(crate) fn read_legacy(
        _data_dir: &Path,
        _name: &str,
        _limit: u64,
    ) -> Result<Option<Vec<u8>>, ErrorCode> {
        Err(ErrorCode::Unavailable)
    }
}

// ===== Bridge =====

/// Opens a session for the plugin the host authenticated as `plugin_id`, its library
/// `plugin_hash`; `external` false for a module the host ships, which needs no grant.
/// Answers 0 when refused.
pub fn storage_session_open(plugin_id: String, plugin_hash: String, external: bool) -> u64 {
    open_session(
        crate::config::datadir(),
        plugin_id,
        plugin_hash.to_ascii_lowercase(),
        external,
    )
}

/// Ends the session: its outstanding requests answer `revoked`, its handle opens
/// nothing again. Never waits for a request.
pub fn storage_session_revoke(session: u64) {
    let removed = SESSIONS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .remove(&session);
    if let Some(session) = removed {
        session.revoke();
    }
}

/// Answers the JSON request on `session`. Blocks on disk and on other sessions'
/// requests: call it off the GUI thread.
pub fn storage_request(session: u64, request: String) -> String {
    let (id, operation) = match parse(&request) {
        Ok(parsed) => parsed,
        Err((id, code)) => return response(&id, Err(code)),
    };
    let Some(session) = lookup(session) else {
        return response(&id, Err(ErrorCode::Revoked));
    };
    let pending = match Pending::register(session, id.clone()) {
        Ok(pending) => pending,
        Err(code) => return response(&id, Err(code)),
    };
    let result = execute(&pending, operation).and_then(|reply| {
        pending.authorize()?;
        Ok(reply)
    });
    response(&id, result)
}

/// Cancels the request `id` on `session`, sent or not yet: it answers `cancelled`.
/// Of the requests not sent yet, the last `MAX_OUTSTANDING` cancelled are remembered.
/// A request already replacing its tree may complete.
pub fn storage_request_cancel(session: u64, id: String) {
    if id.is_empty() || id.len() > MAX_ID_BYTES {
        return;
    }
    if let Some(session) = lookup(session) {
        session.cancel(id);
    }
}

/// Forgets the cancellation of `id` on `session`, once its `storage_request` returned
/// or will never be made.
pub fn storage_request_forget(session: u64, id: String) {
    if let Some(session) = lookup(session) {
        session.forget(&id);
    }
}

/// The session's generation now, to pass to `storage_delivery_error` with the answer
/// of a request sent now. 0 for a closed session.
pub fn storage_session_generation(session: u64) -> u64 {
    lookup(session).map_or(0, |session| session.generation.load(Ordering::SeqCst))
}

/// Why an answer computed at `generation` must not be delivered: `revoked`, `locked`,
/// `denied` when the grant or the key changed since, empty when it may. Reads memory
/// only: safe on the GUI thread.
pub fn storage_delivery_error(session: u64, generation: u64) -> String {
    let code = match lookup(session) {
        None => Some(ErrorCode::Revoked),
        Some(session) => session.delivery_error(generation),
    };
    code.map(ErrorCode::as_str).unwrap_or_default().to_string()
}

#[cfg(all(test, unix))]
#[path = "storage_publication_tests.rs"]
mod publication_tests;

// The backend is `unavailable` but on Unix.
#[cfg(all(test, unix))]
mod tests {
    use std::{
        env, fs,
        os::unix::fs::{symlink, PermissionsExt},
        path::{Path, PathBuf},
        process::{self, Command},
        sync::mpsc,
        thread,
        time::{Duration, Instant},
    };

    use base64::{prelude::BASE64_STANDARD, Engine};
    use serde_json::{json, Value};
    use temp_dir::TempDir;

    use crate::{
        state_crypt::{self, DurableWriteStage},
        storage::{
            disk, lookup, open_session, storage_request, storage_request_cancel,
            storage_request_forget, storage_session_revoke, LogicalPath, Node, Tree, DOCUMENT,
            DOCUMENT_LOCK, DOCUMENT_TEMP, MAX_ENTRIES, MAX_FILE_BYTES, MAX_MESSAGE_BYTES,
            MAX_NAME_BYTES, MAX_OUTSTANDING, MAX_PATH_BYTES, OPERATIONS,
        },
    };

    const PLUGIN: &str = "example";

    /// A built-in session of `PLUGIN` on its own data directory, its storage open.
    struct Fixture {
        dir: TempDir,
        session: u64,
        handle: String,
    }

    impl Fixture {
        fn new() -> Self {
            let dir = TempDir::new().expect("temporary data directory");
            let (session, handle) = session_on(dir.path(), PLUGIN);
            Self {
                dir,
                session,
                handle,
            }
        }

        fn data_dir(&self) -> &Path {
            self.dir.path()
        }

        fn root(&self) -> PathBuf {
            self.data_dir().join(PLUGIN)
        }

        /// Sends `op` on the open storage, `fields` added to it.
        fn call(&self, op: &str, fields: Value) -> Value {
            call(self.session, &self.handle, op, fields)
        }

        fn write(&self, path: &str, data: &[u8], mode: &str) -> Value {
            self.call(
                "write",
                json!({"path": path, "data": BASE64_STANDARD.encode(data), "mode": mode}),
            )
        }

        fn read(&self, path: &str) -> Value {
            self.call("read", json!({"path": path}))
        }
    }

    fn session_on(data_dir: &Path, plugin_id: &str) -> (u64, String) {
        state_crypt::unlock_for_tests();
        let session = open_session(
            data_dir.to_path_buf(),
            plugin_id.into(),
            String::new(),
            false,
        );
        assert_ne!(session, 0, "a built-in session of '{plugin_id}' opens");
        let opened = send(
            session,
            json!({"id": "open", "op": "open", "storage": "private"}),
        );
        assert_eq!(opened["error"], "", "the private storage opens: {opened}");
        let handle = opened["storage"]
            .as_str()
            .expect("a storage handle")
            .to_string();
        (session, handle)
    }

    fn send(session: u64, request: Value) -> Value {
        send_text(session, request.to_string())
    }

    fn send_text(session: u64, request: String) -> Value {
        serde_json::from_str(&storage_request(session, request)).expect("a JSON response")
    }

    fn call(session: u64, handle: &str, op: &str, fields: Value) -> Value {
        let mut request = json!({"id": "r", "op": op, "storage": handle});
        for (key, value) in fields.as_object().expect("request fields") {
            request[key] = value.clone();
        }
        send(session, request)
    }

    fn data(text: &[u8]) -> Value {
        Value::String(BASE64_STANDARD.encode(text))
    }

    fn ok() -> Value {
        json!({"id": "r", "error": ""})
    }

    fn failed(code: &str) -> Value {
        json!({"id": "r", "error": code})
    }

    fn entry(path: &str, kind: &str, size: u64) -> Value {
        json!({"path": path, "kind": kind, "size": size})
    }

    /// Seals `tree` as the tree of `PLUGIN` in `data_dir`, bypassing the request bounds.
    fn seed(data_dir: &Path, tree: &Tree) {
        let text = tree.encode(PLUGIN).expect("seed tree");
        let sealed = state_crypt::seal(&text).expect("sealed seed");
        disk::replace(data_dir, PLUGIN, &sealed, || Ok(())).expect("seeded tree");
    }

    /// Seals `text` as the tree of `PLUGIN` in its root, as found on disk.
    fn plant(f: &Fixture, text: &str) -> Vec<u8> {
        let sealed = state_crypt::seal(text).expect("sealed tree");
        fs::create_dir_all(f.root()).expect("plugin root");
        fs::write(f.root().join(DOCUMENT), &sealed).expect("planted tree");
        sealed
    }

    /// Writes `data` as the legacy blob of `plugin_id`, as the single-blob API saved it.
    fn legacy_blob(data_dir: &Path, plugin_id: &str, data: &[u8]) -> PathBuf {
        let path = state_crypt::legacy_plugin_state_path(data_dir, plugin_id).expect("legacy path");
        state_crypt::write_state(&path, &BASE64_STANDARD.encode(data)).expect("legacy blob");
        path
    }

    fn wait_running(session: u64, count: usize) {
        let start = Instant::now();
        loop {
            let running = lookup(session)
                .map(|session| session.requests().running.len())
                .unwrap_or_default();
            if running == count {
                return;
            }
            assert!(
                start.elapsed() < Duration::from_secs(10),
                "{count} requests in flight, got {running}"
            );
            thread::sleep(Duration::from_millis(5));
        }
    }

    // ===== Operations =====

    #[test]
    fn every_operation_on_nested_directories_and_independent_files() {
        let f = Fixture::new();
        assert_eq!(
            send(f.session, json!({"id": "d", "op": "discover"})),
            json!({"id": "d", "error": "", "storages": [{"id": "private", "name": "Private plugin storage"}]})
        );
        assert_eq!(
            send(
                f.session,
                json!({"id": "o", "op": "open", "storage": "shared"})
            ),
            json!({"id": "o", "error": "not_found"}),
            "only the private storage exists"
        );
        assert_eq!(
            f.call("inspect", json!({})),
            json!({"id": "r", "error": "", "entry": entry("", "directory", 0)})
        );
        assert_eq!(
            f.call("list", json!({})),
            json!({"id": "r", "error": "", "entries": [], "next": ""})
        );
        assert!(!f.root().exists(), "reading creates nothing on disk");

        assert_eq!(f.call("mkdir", json!({"path": "a"})), ok());
        assert_eq!(f.call("mkdir", json!({"path": "a/b"})), ok());
        assert_eq!(
            f.call("mkdir", json!({"path": "x/y"})),
            failed("not_found"),
            "no parent"
        );
        assert_eq!(f.write("a/b/one.txt", b"one", "create"), ok());
        assert_eq!(f.write("a/two.txt", b"\x00two", "create"), ok());

        assert_eq!(
            f.read("a/b/one.txt"),
            json!({"id": "r", "error": "", "data": data(b"one")})
        );
        assert_eq!(
            f.read("a/two.txt"),
            json!({"id": "r", "error": "", "data": data(b"\x00two")})
        );
        assert_eq!(
            f.call("inspect", json!({"path": "a/b/one.txt"})),
            json!({"id": "r", "error": "", "entry": entry("a/b/one.txt", "file", 3)})
        );
        assert_eq!(
            f.call("inspect", json!({"path": "a"})),
            json!({"id": "r", "error": "", "entry": entry("a", "directory", 0)})
        );
        assert_eq!(
            f.call("list", json!({"path": ""})),
            json!({"id": "r", "error": "", "entries": [entry("a", "directory", 0)], "next": ""})
        );
        assert_eq!(
            f.call("list", json!({"path": "a"})),
            json!({"id": "r", "error": "", "entries": [entry("a/b", "directory", 0), entry("a/two.txt", "file", 4)], "next": ""})
        );

        assert_eq!(f.call("rename", json!({"path": "a/b", "to": "c"})), ok());
        assert_eq!(
            f.read("c/one.txt"),
            json!({"id": "r", "error": "", "data": data(b"one")})
        );
        assert_eq!(f.read("a/b/one.txt"), failed("not_found"));
        assert_eq!(
            f.call("rename", json!({"path": "a/two.txt", "to": "c/two.txt"})),
            ok()
        );
        assert_eq!(
            f.read("c/two.txt"),
            json!({"id": "r", "error": "", "data": data(b"\x00two")})
        );

        assert_eq!(f.call("remove", json!({"path": "c"})), failed("not_empty"));
        assert_eq!(
            f.read("c/one.txt"),
            json!({"id": "r", "error": "", "data": data(b"one")})
        );
        assert_eq!(
            f.call("remove", json!({"path": "c", "recursive": true})),
            ok()
        );
        assert_eq!(f.call("inspect", json!({"path": "c"})), failed("not_found"));
        assert_eq!(
            f.call("inspect", json!({"path": "c/one.txt"})),
            failed("not_found")
        );
        assert_eq!(
            f.call("remove", json!({"path": "a"})),
            ok(),
            "an empty directory needs no recursion"
        );
        assert_eq!(
            f.call("list", json!({})),
            json!({"id": "r", "error": "", "entries": [], "next": ""})
        );
    }

    #[test]
    fn write_modes_renames_and_removals_refuse_explicitly() {
        let f = Fixture::new();
        assert_eq!(f.write("f", b"first", "replace"), failed("not_found"));
        assert_eq!(f.write("f", b"first", "create"), ok());
        assert_eq!(f.write("f", b"second", "create"), failed("already_exists"));
        assert_eq!(
            f.read("f"),
            json!({"id": "r", "error": "", "data": data(b"first")})
        );
        assert_eq!(f.write("f", b"third", "replace"), ok());
        assert_eq!(f.write("g", b"new", "upsert"), ok());
        assert_eq!(f.write("g", b"newer", "upsert"), ok());
        assert_eq!(
            f.read("f"),
            json!({"id": "r", "error": "", "data": data(b"third")})
        );
        assert_eq!(
            f.read("g"),
            json!({"id": "r", "error": "", "data": data(b"newer")})
        );
        assert_eq!(f.write("e", b"", "create"), ok(), "an empty file");
        assert_eq!(f.read("e"), json!({"id": "r", "error": "", "data": ""}));

        assert_eq!(f.call("mkdir", json!({"path": "d"})), ok());
        assert_eq!(
            f.call("mkdir", json!({"path": "d"})),
            failed("already_exists")
        );
        assert_eq!(
            f.call("mkdir", json!({"path": "f"})),
            failed("already_exists")
        );
        for mode in ["create", "replace", "upsert"] {
            assert_eq!(
                f.write("d", b"x", mode),
                failed("is_directory"),
                "{mode} on a directory"
            );
        }
        assert_eq!(f.read("d"), failed("is_directory"));
        assert_eq!(f.write("f/x", b"x", "upsert"), failed("not_directory"));
        assert_eq!(
            f.call("mkdir", json!({"path": "f/x"})),
            failed("not_directory")
        );
        assert_eq!(
            f.call("list", json!({"path": "f"})),
            failed("not_directory")
        );
        assert_eq!(
            f.call("inspect", json!({"path": "f/x"})),
            failed("not_directory")
        );
        assert_eq!(
            f.call("list", json!({"path": "missing"})),
            failed("not_found")
        );

        assert_eq!(
            f.call("rename", json!({"path": "f", "to": "g"})),
            failed("already_exists")
        );
        assert_eq!(
            f.read("f"),
            json!({"id": "r", "error": "", "data": data(b"third")})
        );
        assert_eq!(
            f.read("g"),
            json!({"id": "r", "error": "", "data": data(b"newer")})
        );
        assert_eq!(
            f.call("rename", json!({"path": "f", "to": "f"})),
            failed("invalid_path")
        );
        assert_eq!(
            f.call("rename", json!({"path": "d", "to": "d/inner"})),
            failed("invalid_path")
        );
        assert_eq!(
            f.call("rename", json!({"path": "missing", "to": "h"})),
            failed("not_found")
        );
        assert_eq!(
            f.call("rename", json!({"path": "f", "to": "missing/f"})),
            failed("not_found")
        );
        assert_eq!(
            f.call("rename", json!({"path": "f", "to": "g/f"})),
            failed("not_directory")
        );
        assert_eq!(f.call("rename", json!({"path": "f", "to": "d/f"})), ok());
        assert_eq!(
            f.read("d/f"),
            json!({"id": "r", "error": "", "data": data(b"third")})
        );

        assert_eq!(
            f.call("remove", json!({"path": "missing"})),
            failed("not_found")
        );
        assert_eq!(
            f.call("remove", json!({"path": ""})),
            failed("invalid_path")
        );
        assert_eq!(f.call("mkdir", json!({"path": ""})), failed("invalid_path"));
        assert_eq!(
            f.call("rename", json!({"path": "", "to": "z"})),
            failed("invalid_path")
        );
        assert_eq!(f.write("", b"x", "upsert"), failed("invalid_path"));
        assert_eq!(f.read(""), failed("invalid_path"));
        assert_eq!(
            f.call("remove", json!({"path": "g", "recursive": true})),
            ok(),
            "recursive removes a file"
        );
        assert_eq!(f.read("g"), failed("not_found"));
    }

    #[test]
    fn listing_pages_by_name_and_skips_deeper_entries() {
        let f = Fixture::new();
        assert_eq!(f.call("mkdir", json!({"path": "p"})), ok());
        for name in ["f3", "f0", "f2", "f1"] {
            assert_eq!(
                f.write(&format!("p/{name}"), name.as_bytes(), "create"),
                ok()
            );
        }
        assert_eq!(f.call("mkdir", json!({"path": "p/sub"})), ok());
        assert_eq!(f.write("p/sub/deep", b"deep", "create"), ok());
        assert_eq!(f.write("p-sibling", b"s", "create"), ok());

        assert_eq!(
            f.call("list", json!({"path": "p", "limit": 2})),
            json!({"id": "r", "error": "", "entries": [entry("p/f0", "file", 2), entry("p/f1", "file", 2)], "next": "f1"})
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "limit": 2, "after": "f1"})),
            json!({"id": "r", "error": "", "entries": [entry("p/f2", "file", 2), entry("p/f3", "file", 2)], "next": "f3"})
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "limit": 2, "after": "f3"})),
            json!({"id": "r", "error": "", "entries": [entry("p/sub", "directory", 0)], "next": ""})
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "after": "f0"})),
            json!({"id": "r", "error": "", "entries": [
                entry("p/f1", "file", 2), entry("p/f2", "file", 2), entry("p/f3", "file", 2), entry("p/sub", "directory", 0)
            ], "next": ""}),
            "after a name that exists or not, without a limit"
        );
        assert_eq!(
            f.call("list", json!({})),
            json!({"id": "r", "error": "", "entries": [entry("p", "directory", 0), entry("p-sibling", "file", 1)], "next": ""})
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "limit": 0})),
            failed("invalid_request")
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "limit": 129})),
            failed("invalid_request")
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "limit": -1})),
            failed("invalid_request")
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "after": ".."})),
            failed("invalid_path")
        );
        assert_eq!(
            f.call("list", json!({"path": "p", "after": "a/b"})),
            failed("invalid_path")
        );
    }

    // ===== Requests =====

    #[test]
    fn malformed_and_oversized_requests_are_refused() {
        let f = Fixture::new();
        let unidentified = json!({"id": "", "error": "invalid_request"});
        assert_eq!(send_text(f.session, "not json".into()), unidentified);
        assert_eq!(send_text(f.session, "[]".into()), unidentified);
        assert_eq!(
            send(f.session, json!({"op": "discover"})),
            unidentified,
            "no id"
        );
        assert_eq!(
            send(f.session, json!({"id": "", "op": "discover"})),
            unidentified
        );
        assert_eq!(
            send(f.session, json!({"id": 7, "op": "discover"})),
            unidentified
        );
        assert_eq!(
            send(f.session, json!({"id": "x".repeat(257), "op": "discover"})),
            unidentified
        );
        let refused = |request: Value| {
            assert_eq!(
                send(f.session, request.clone()),
                failed("invalid_request"),
                "{request}"
            );
        };
        refused(json!({"id": "r"}));
        refused(json!({"id": "r", "op": "format"}));
        refused(json!({"id": "r", "op": "discover", "plugin": "other"}));
        refused(json!({"id": "r", "op": "list", "storage": f.handle, "limit": null}));
        refused(json!({"id": "r", "op": "list", "storage": f.handle, "after": null}));
        refused(json!({"id": "r", "op": "read", "storage": f.handle, "path": "f", "offset": 0}));
        refused(json!({"id": "r", "op": "read", "storage": f.handle, "path": "f", "data": ""}));
        refused(json!({"id": "r", "op": "read", "storage": f.handle}));
        refused(json!({"id": "r", "op": "read", "path": "f"}));
        refused(json!({"id": "r", "op": "write", "storage": f.handle, "path": "f", "data": ""}));
        refused(
            json!({"id": "r", "op": "write", "storage": f.handle, "path": "f", "data": "", "mode": "append"}),
        );
        refused(
            json!({"id": "r", "op": "write", "storage": f.handle, "path": "f", "data": "!!", "mode": "create"}),
        );
        refused(
            json!({"id": "r", "op": "remove", "storage": f.handle, "path": "f", "recursive": "yes"}),
        );

        let padding = "x".repeat(MAX_MESSAGE_BYTES);
        assert_eq!(
            send(
                f.session,
                json!({"id": "r", "op": "discover", "padding": padding})
            ),
            json!({"id": "", "error": "too_large"}),
            "refused before it is read"
        );
        assert_eq!(
            f.write("big", &vec![1; MAX_FILE_BYTES + 1], "create"),
            failed("too_large")
        );
        assert_eq!(f.read("big"), failed("not_found"), "nothing published");
        let largest = vec![2; MAX_FILE_BYTES];
        assert_eq!(f.write("big", &largest, "create"), ok());
        assert_eq!(
            f.read("big"),
            json!({"id": "r", "error": "", "data": data(&largest)})
        );
    }
    #[test]
    fn paths_leaving_the_tree_are_refused() {
        let f = Fixture::new();
        assert_eq!(f.call("mkdir", json!({"path": "a"})), ok());
        let longest_name = "n".repeat(MAX_NAME_BYTES);
        assert_eq!(f.write(&format!("a/{longest_name}"), b"x", "create"), ok());
        let long_path = vec!["n".repeat(250); 17].join("/");
        for path in [
            "/a",
            "a/",
            "a//b",
            ".",
            "..",
            "../a",
            "a/../b",
            "a/./b",
            "a/..",
            "a\u{0}b",
            "a\nb",
            "a\u{7f}b",
            "a\\b",
            "..\\a",
            "c:",
            "C:/a",
            "a/b:stream",
            &"n".repeat(MAX_NAME_BYTES + 1),
            &long_path,
        ] {
            assert_eq!(
                f.write(path, b"x", "upsert"),
                failed("invalid_path"),
                "write {path:?}"
            );
            assert_eq!(f.read(path), failed("invalid_path"), "read {path:?}");
            assert_eq!(
                f.call("mkdir", json!({"path": path})),
                failed("invalid_path"),
                "mkdir {path:?}"
            );
            assert_eq!(
                f.call("inspect", json!({"path": path})),
                failed("invalid_path"),
                "inspect {path:?}"
            );
            assert_eq!(
                f.call("rename", json!({"path": "a", "to": path})),
                failed("invalid_path"),
                "rename to {path:?}"
            );
        }
        let names: Vec<String> = fs::read_dir(f.data_dir())
            .expect("data directory")
            .map(|entry| {
                entry
                    .expect("entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect();
        assert_eq!(
            names,
            vec![PLUGIN.to_string()],
            "nothing but the plugin root in the data directory"
        );
    }

    #[test]
    fn names_are_unicode_case_sensitive_and_never_normalized() {
        let f = Fixture::new();
        for (name, content) in [
            ("note", &b"lower"[..]),
            ("Note", b"upper"),
            ("caf\u{e9}", b"composed"),
            ("cafe\u{301}", b"decomposed"),
        ] {
            assert_eq!(f.write(name, content, "create"), ok(), "{name:?}");
        }
        assert_eq!(
            f.read("Note"),
            json!({"id": "r", "error": "", "data": data(b"upper")})
        );
        assert_eq!(
            f.read("cafe\u{301}"),
            json!({"id": "r", "error": "", "data": data(b"decomposed")})
        );
        assert_eq!(
            f.call("list", json!({})),
            json!({"id": "r", "error": "", "entries": [
                entry("Note", "file", 5),
                entry("cafe\u{301}", "file", 10),
                entry("caf\u{e9}", "file", 8),
                entry("note", "file", 5),
            ], "next": ""}),
            "four names, ordered byte by byte"
        );
    }

    // ===== Sessions =====

    #[test]
    fn handles_are_scoped_to_their_session_and_trees_to_their_plugin() {
        let f = Fixture::new();
        let (other, other_handle) = session_on(f.data_dir(), PLUGIN);
        assert_ne!(f.handle, other_handle, "each session its own handle");
        assert_eq!(f.write("shared", b"one tree", "create"), ok());
        assert_eq!(
            call(other, &other_handle, "read", json!({"path": "shared"})),
            json!({"id": "r", "error": "", "data": data(b"one tree")}),
            "modules of one plugin share its tree"
        );
        assert_eq!(
            call(other, &f.handle, "read", json!({"path": "shared"})),
            failed("invalid_handle")
        );
        assert_eq!(
            call(other, "private", "read", json!({"path": "shared"})),
            failed("invalid_handle")
        );
        assert_eq!(
            call(other, "", "mkdir", json!({"path": "d"})),
            failed("invalid_handle")
        );

        let (stranger, stranger_handle) = session_on(f.data_dir(), "stranger");
        assert_eq!(
            call(stranger, &f.handle, "read", json!({"path": "shared"})),
            failed("invalid_handle")
        );
        assert_eq!(
            call(
                stranger,
                &stranger_handle,
                "read",
                json!({"path": "shared"})
            ),
            failed("not_found")
        );
        assert_eq!(
            call(stranger, &stranger_handle, "mkdir", json!({"path": "mine"})),
            ok()
        );
        assert_eq!(
            f.call("inspect", json!({"path": "mine"})),
            failed("not_found")
        );
        assert!(
            f.data_dir().join("stranger").join(DOCUMENT).is_file(),
            "its own root"
        );
    }

    #[test]
    fn a_session_after_restart_reads_the_same_tree_and_a_revoked_one_nothing() {
        let f = Fixture::new();
        assert_eq!(f.call("mkdir", json!({"path": "kept"})), ok());
        assert_eq!(f.write("kept/state.json", b"{\"n\":1}", "create"), ok());
        storage_session_revoke(f.session);
        for request in [
            json!({"id": "r", "op": "discover"}),
            json!({"id": "r", "op": "open", "storage": "private"}),
            json!({"id": "r", "op": "read", "storage": f.handle, "path": "kept/state.json"}),
        ] {
            assert_eq!(
                send(f.session, request.clone()),
                failed("revoked"),
                "{request}"
            );
        }
        let (session, handle) = session_on(f.data_dir(), PLUGIN);
        assert_eq!(
            call(session, &handle, "read", json!({"path": "kept/state.json"})),
            json!({"id": "r", "error": "", "data": data(b"{\"n\":1}")})
        );
        assert_eq!(
            call(
                session,
                &f.handle,
                "read",
                json!({"path": "kept/state.json"})
            ),
            failed("invalid_handle")
        );
    }
    #[test]
    fn host_entries_and_invalid_ids_get_no_session() {
        let dir = TempDir::new().expect("temporary data directory");
        let open = |plugin_id: &str, external: bool| {
            open_session(
                dir.path().to_path_buf(),
                plugin_id.into(),
                if external { "aa".into() } else { String::new() },
                external,
            )
        };
        let longest = "p".repeat(MAX_NAME_BYTES);
        let too_long = "p".repeat(MAX_NAME_BYTES + 1);
        for plugin_id in [
            "keys",
            "KEYS",
            "Plugins",
            "plugin-state",
            "musig-sessions",
            "locked",
            "Locked",
            "unlock.key",
            "unlock.bip138",
            "app.bip138",
            "APP.BIP138",
            "hwi.bip138",
            "app.tmp",
            "keys.lock",
            "0123456789abcdef0123456789abcdef",
            "0123456789ABCDEF0123456789ABCDEF",
            "con",
            "CON",
            "nul.txt",
            "Com1",
            "lpt9.log",
            "plugin.",
            too_long.as_str(),
            "",
            "../x",
            ".hidden",
            "a/b",
            "a\\b",
            "c:",
        ] {
            assert_eq!(open(plugin_id, false), 0, "no session for {plugin_id:?}");
            assert_eq!(
                open(plugin_id, true),
                0,
                "no external session for {plugin_id:?}"
            );
        }
        // Reserved by name: an empty directory of that name changes nothing.
        fs::create_dir(dir.path().join("locked")).expect("empty reserved root");
        assert_eq!(open("locked", false), 0, "still reserved");
        for plugin_id in [
            PLUGIN,
            "my.plugin",
            "console",
            "com10",
            "0123456789abcdef0123456789abcde",
            "keystore",
            longest.as_str(),
        ] {
            assert_ne!(open(plugin_id, false), 0, "a session for {plugin_id:?}");
        }
        assert_eq!(
            open_session(dir.path().to_path_buf(), PLUGIN.into(), String::new(), true),
            0,
            "an external plugin needs its library hash"
        );
        assert_eq!(
            send(0, json!({"id": "r", "op": "discover"})),
            failed("revoked"),
            "0 is never a session"
        );
    }

    // ===== Disk =====

    #[test]
    fn the_tree_is_one_sealed_document_below_the_visible_plugin_root() {
        let f = Fixture::new();
        assert_eq!(f.call("mkdir", json!({"path": "secret-directory"})), ok());
        assert_eq!(
            f.write("secret-directory/secret-name", b"secret-content", "create"),
            ok()
        );
        let mut names: Vec<String> = fs::read_dir(f.root())
            .expect("plugin root")
            .map(|entry| {
                entry
                    .expect("entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect();
        names.sort();
        assert_eq!(
            names,
            vec![DOCUMENT.to_string(), DOCUMENT_LOCK.to_string()],
            "names never reach the disk"
        );
        let sealed = fs::read(f.root().join(DOCUMENT)).expect("sealed tree");
        assert!(state_crypt::is_sealed(&sealed), "a BIP138 envelope");
        for clear in [
            &b"secret"[..],
            BASE64_STANDARD.encode(b"secret-content").as_bytes(),
        ] {
            assert!(
                !sealed.windows(clear.len()).any(|window| window == clear),
                "nothing in clear"
            );
        }
        let mode = fs::metadata(f.root()).expect("root").permissions().mode() & 0o777;
        assert_eq!(mode, 0o700, "a private root");
        let mode = fs::metadata(f.root().join(DOCUMENT))
            .expect("tree")
            .permissions()
            .mode()
            & 0o777;
        assert_eq!(mode, 0o600, "a private tree");
    }
    #[test]
    fn a_corrupt_tree_is_refused_and_never_replaced() {
        let f = Fixture::new();
        assert_eq!(f.write("f", b"x", "create"), ok());
        let document = f.root().join(DOCUMENT);
        let tree = |owner: &str, rest: &str| {
            state_crypt::seal(&format!(
                r#"{{"version":1,"owner":"{owner}","migrated":false,{rest}}}"#
            ))
            .expect("sealed")
        };
        let cases = [
            (
                "unsealed",
                br#"{"version":1,"owner":"example","migrated":false,"entries":[]}"#.to_vec(),
            ),
            ("garbage", state_crypt::seal("not json").expect("sealed")),
            (
                "version",
                state_crypt::seal(
                    r#"{"version":2,"owner":"example","migrated":false,"entries":[]}"#,
                )
                .expect("sealed"),
            ),
            (
                "no owner",
                state_crypt::seal(r#"{"version":1,"migrated":false,"entries":[]}"#)
                    .expect("sealed"),
            ),
            ("another plugin's", tree("other", r#""entries":[]"#)),
            ("another case", tree("Example", r#""entries":[]"#)),
            (
                "orphan",
                tree(
                    PLUGIN,
                    r#""entries":[{"kind":"file","path":"no/parent","data":""}]"#,
                ),
            ),
            (
                "file parent",
                tree(
                    PLUGIN,
                    r#""entries":[{"kind":"file","path":"f","data":""},{"kind":"file","path":"f/g","data":""}]"#,
                ),
            ),
            (
                "duplicate",
                tree(
                    PLUGIN,
                    r#""entries":[{"kind":"directory","path":"d"},{"kind":"directory","path":"d"}]"#,
                ),
            ),
            (
                "traversal",
                tree(PLUGIN, r#""entries":[{"kind":"directory","path":".."}]"#),
            ),
            (
                "native path",
                tree(PLUGIN, r#""entries":[{"kind":"directory","path":"C:"}]"#),
            ),
            ("unknown field", tree(PLUGIN, r#""entries":[],"extra":0"#)),
        ];
        for (case, bytes) in cases {
            fs::write(&document, &bytes).expect("corrupt tree");
            assert_eq!(
                send(
                    f.session,
                    json!({"id": "r", "op": "open", "storage": "private"})
                ),
                failed("corrupt"),
                "{case}: open"
            );
            assert_eq!(f.read("f"), failed("corrupt"), "{case}: read");
            assert_eq!(f.call("list", json!({})), failed("corrupt"), "{case}: list");
            assert_eq!(
                f.write("g", b"y", "create"),
                failed("corrupt"),
                "{case}: write"
            );
            assert_eq!(
                f.call("migrate", json!({"path": "m"})),
                failed("corrupt"),
                "{case}: migrate"
            );
            assert_eq!(
                fs::read(&document).expect("tree"),
                bytes,
                "{case}: left as found"
            );
        }
        plant(&f, &tree_text(PLUGIN));
        assert_eq!(
            f.call("list", json!({})),
            json!({"id": "r", "error": "", "entries": [], "next": ""})
        );
    }

    /// The text of an empty tree of `owner`.
    fn tree_text(owner: &str) -> String {
        Tree::default().encode(owner).expect("empty tree")
    }

    #[test]
    fn a_tree_copied_from_another_plugin_is_refused() {
        let f = Fixture::new();
        let (other, other_handle) = session_on(f.data_dir(), "other");
        assert_eq!(
            call(
                other,
                &other_handle,
                "write",
                json!({"path": "secret", "data": data(b"other's"), "mode": "create"})
            ),
            ok()
        );
        fs::create_dir(f.root()).expect("plugin root");
        fs::copy(
            f.data_dir().join("other").join(DOCUMENT),
            f.root().join(DOCUMENT),
        )
        .expect("copied tree");
        assert_eq!(
            send(
                f.session,
                json!({"id": "r", "op": "open", "storage": "private"})
            ),
            failed("corrupt")
        );
        assert_eq!(f.read("secret"), failed("corrupt"));
        assert_eq!(
            f.write("secret", b"mine", "upsert"),
            failed("corrupt"),
            "never written over"
        );
        assert_eq!(
            call(other, &other_handle, "read", json!({"path": "secret"})),
            json!({"id": "r", "error": "", "data": data(b"other's")})
        );
    }

    #[test]
    fn entry_and_tree_size_bounds_refuse_before_publication() {
        let f = Fixture::new();
        let mut tree = Tree::default();
        for index in 0..MAX_ENTRIES {
            tree.nodes
                .insert(LogicalPath(vec![format!("d{index}")]), Node::Directory);
        }
        seed(f.data_dir(), &tree);
        assert_eq!(
            f.call("mkdir", json!({"path": "one-more"})),
            failed("too_large")
        );
        assert_eq!(f.write("one-more", b"x", "upsert"), failed("too_large"));
        assert_eq!(
            f.call("rename", json!({"path": "d0", "to": "renamed"})),
            ok(),
            "a rename adds nothing"
        );
        assert_eq!(f.call("remove", json!({"path": "renamed"})), ok());
        assert_eq!(
            f.call("mkdir", json!({"path": "one-more"})),
            ok(),
            "room again"
        );

        let f = Fixture::new();
        let mut tree = Tree::default();
        // Base64 of eleven largest files stays below the tree bound, of twelve it does not.
        for index in 0..11 {
            tree.nodes.insert(
                LogicalPath(vec![format!("f{index}")]),
                Node::File(vec![index; MAX_FILE_BYTES]),
            );
        }
        seed(f.data_dir(), &tree);
        let before = fs::read(f.root().join(DOCUMENT)).expect("tree");
        assert_eq!(
            f.write("f11", &vec![11; MAX_FILE_BYTES], "create"),
            failed("too_large")
        );
        assert_eq!(
            fs::read(f.root().join(DOCUMENT)).expect("tree"),
            before,
            "nothing published"
        );
        assert_eq!(
            f.write("f0", b"small", "replace"),
            ok(),
            "a smaller tree is fine"
        );
    }
    #[test]
    fn links_in_the_data_directory_are_never_followed() {
        let f = Fixture::new();
        let elsewhere = TempDir::new().expect("outside directory");
        symlink(elsewhere.path(), f.root()).expect("root link");
        let open = |session: u64| {
            send(
                session,
                json!({"id": "r", "op": "open", "storage": "private"}),
            )
        };
        assert_eq!(
            open(f.session),
            failed("unavailable"),
            "open refuses a linked root"
        );
        assert_eq!(f.read("f"), failed("unavailable"));
        assert_eq!(f.write("f", b"x", "create"), failed("unavailable"));
        assert_eq!(f.call("mkdir", json!({"path": "d"})), failed("unavailable"));
        assert_eq!(
            fs::read_dir(elsewhere.path()).expect("outside").count(),
            0,
            "nothing written through the link"
        );
        fs::remove_file(f.root()).expect("drop the root link");

        let outside = elsewhere.path().join("outside");
        fs::write(&outside, b"outside").expect("outside file");
        fs::create_dir(f.root()).expect("real root");
        symlink(&outside, f.root().join(DOCUMENT)).expect("document link");
        assert_eq!(
            open(f.session),
            failed("unavailable"),
            "open refuses a linked tree"
        );
        assert_eq!(f.read("f"), failed("unavailable"));
        assert_eq!(f.write("f", b"x", "create"), failed("unavailable"));
        fs::remove_file(f.root().join(DOCUMENT)).expect("drop the document link");

        symlink(&outside, f.root().join(DOCUMENT_TEMP)).expect("temp link");
        assert_eq!(
            f.write("f", b"x", "create"),
            ok(),
            "a planted temp is unlinked"
        );
        assert_eq!(
            fs::read(&outside).expect("outside"),
            b"outside",
            "never written through"
        );
        assert!(
            fs::symlink_metadata(f.root().join(DOCUMENT_TEMP)).is_err(),
            "no temp left"
        );
        assert_eq!(
            f.read("f"),
            json!({"id": "r", "error": "", "data": data(b"x")})
        );

        let file_root = TempDir::new().expect("another data directory");
        fs::write(file_root.path().join(PLUGIN), b"").expect("a file in place of the root");
        let session = open_session(
            file_root.path().to_path_buf(),
            PLUGIN.into(),
            String::new(),
            false,
        );
        assert_eq!(open(session), failed("unavailable"), "a file is no root");
    }

    #[test]
    fn a_legacy_blob_is_never_read_through_a_link() {
        let f = Fixture::new();
        let elsewhere = TempDir::new().expect("outside directory");
        let outside = legacy_blob(elsewhere.path(), PLUGIN, b"outside");
        let name = outside.file_name().expect("blob name").to_owned();

        symlink(
            elsewhere.path().join(state_crypt::PLUGIN_STATE_DIR),
            f.data_dir().join(state_crypt::PLUGIN_STATE_DIR),
        )
        .expect("legacy directory link");
        assert_eq!(
            f.call("migrate", json!({"path": "m"})),
            failed("unavailable")
        );
        fs::remove_file(f.data_dir().join(state_crypt::PLUGIN_STATE_DIR)).expect("drop the link");

        fs::create_dir(f.data_dir().join(state_crypt::PLUGIN_STATE_DIR)).expect("legacy directory");
        symlink(
            &outside,
            f.data_dir().join(state_crypt::PLUGIN_STATE_DIR).join(&name),
        )
        .expect("blob link");
        assert_eq!(
            f.call("migrate", json!({"path": "m"})),
            failed("unavailable")
        );
        assert_eq!(f.read("m"), failed("not_found"), "nothing imported");
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_fifo_in_place_of_the_tree_does_not_hang() {
        use rustix::fs::{mknodat, FileType, Mode, CWD};

        let f = Fixture::new();
        fs::create_dir(f.root()).expect("root");
        mknodat(
            CWD,
            f.root().join(DOCUMENT).as_path(),
            FileType::Fifo,
            Mode::RUSR | Mode::WUSR,
            0,
        )
        .expect("fifo");
        assert_eq!(f.read("f"), failed("unavailable"));
    }

    #[test]
    fn an_interrupted_write_leaves_the_previous_or_the_new_tree() {
        for stage in [
            DurableWriteStage::TempPartial,
            DurableWriteStage::TempWritten,
            DurableWriteStage::TempSynced,
            DurableWriteStage::Renamed,
        ] {
            let f = Fixture::new();
            assert_eq!(f.write("f", b"old", "create"), ok());
            state_crypt::fail_durable_write_at(stage);
            assert_eq!(
                f.write("f", b"new", "replace"),
                failed("io"),
                "interrupted at {stage:?}"
            );
            let expected: &[u8] = if stage == DurableWriteStage::Renamed {
                b"new"
            } else {
                b"old"
            };
            assert_eq!(
                f.read("f"),
                json!({"id": "r", "error": "", "data": data(expected)}),
                "a complete tree after {stage:?}"
            );
            assert_eq!(
                f.write("f", b"newer", "replace"),
                ok(),
                "writes again after {stage:?}"
            );
            assert_eq!(
                f.read("f"),
                json!({"id": "r", "error": "", "data": data(b"newer")})
            );
            assert!(
                !f.root().join(DOCUMENT_TEMP).exists(),
                "no temp left after {stage:?}"
            );
        }
    }

    #[test]
    fn concurrent_writer_process() {
        let Some(dir) = env::var_os("SILENT_STORAGE_CONCURRENT_DIR") else {
            return;
        };
        let dir = PathBuf::from(dir);
        let name = env::var("SILENT_STORAGE_CONCURRENT_NAME").expect("writer file name");
        let coordination = PathBuf::from(
            env::var_os("SILENT_STORAGE_COORDINATION").expect("coordination directory"),
        );
        state_crypt::unlock_for_tests();
        let session = open_session(dir, PLUGIN.into(), String::new(), false);
        if name == "first" {
            let coordination = coordination.clone();
            state_crypt::on_durable_write_at(DurableWriteStage::TempPartial, move || {
                fs::write(coordination.join("partial"), b"ready").expect("partial-write signal");
                wait_file(&coordination.join("release"));
            });
        }
        let started = Instant::now();
        let handle = loop {
            let opened = send(
                session,
                json!({"id":"open", "op":"open", "storage":"private"}),
            );
            if opened["error"] == "busy" {
                fs::write(coordination.join("busy"), b"busy").expect("contention signal");
                assert!(
                    started.elapsed() < Duration::from_secs(10),
                    "writer opens after lock release"
                );
                thread::sleep(Duration::from_millis(5));
                continue;
            }
            assert_eq!(
                opened["error"], "",
                "writer opens a complete tree: {opened}"
            );
            break opened["storage"]
                .as_str()
                .expect("writer handle")
                .to_string();
        };
        let request = json!({"id":"write", "op":"write", "storage": handle, "path":name,
            "mode":"create", "data":BASE64_STANDARD.encode(vec![7; MAX_FILE_BYTES])});
        loop {
            let written = send(session, request.clone());
            if written["error"] == "busy" {
                fs::write(coordination.join("busy"), b"busy").expect("contention signal");
                assert!(
                    started.elapsed() < Duration::from_secs(10),
                    "writer finishes after lock release"
                );
                thread::sleep(Duration::from_millis(5));
                continue;
            }
            assert_eq!(
                written["error"], "",
                "independent whole file commits: {written}"
            );
            break;
        }
        storage_session_revoke(session);
    }

    fn wait_file(path: &Path) {
        let started = Instant::now();
        while !path.exists() {
            assert!(
                started.elapsed() < Duration::from_secs(10),
                "process reaches {}",
                path.display()
            );
            thread::sleep(Duration::from_millis(5));
        }
    }

    #[test]
    fn overlapping_processes_keep_both_files_and_recover_a_killed_partial_writer() {
        for kill in [false, true] {
            let f = Fixture::new();
            assert_eq!(f.write("anchor", b"kept", "create"), ok());
            let coordination = TempDir::new().expect("process coordination directory");
            let spawn = |name: &str| {
                Command::new(env::current_exe().expect("test executable"))
                    .args([
                        "--exact",
                        "storage::tests::concurrent_writer_process",
                        "--nocapture",
                    ])
                    .env("SILENT_STORAGE_CONCURRENT_DIR", f.data_dir())
                    .env("SILENT_STORAGE_CONCURRENT_NAME", name)
                    .env("SILENT_STORAGE_COORDINATION", coordination.path())
                    .spawn()
                    .expect("spawn independent tree writer")
            };
            let mut first = spawn("first");
            wait_file(&coordination.path().join("partial"));
            let partial =
                fs::read(f.root().join(DOCUMENT_TEMP)).expect("actual partial encrypted temp");
            assert!(
                !partial.is_empty() && partial.len() < MAX_FILE_BYTES,
                "first process is inside a partial write"
            );
            let mut second = spawn("second");
            wait_file(&coordination.path().join("busy"));
            assert!(
                second.try_wait().expect("second process state").is_none(),
                "second writer cannot publish the first writer's temp"
            );
            if kill {
                first
                    .kill()
                    .expect("kill the process holding the partial temp and lock");
                assert!(
                    !first.wait().expect("killed process status").success(),
                    "the first writer was interrupted"
                );
            } else {
                fs::write(coordination.path().join("release"), b"continue")
                    .expect("release the first writer");
                assert!(
                    first.wait().expect("first writer status").success(),
                    "first independent file commits"
                );
            }
            assert!(
                second.wait().expect("second writer status").success(),
                "second writer reloads under the released lock"
            );
            assert_eq!(
                f.read("anchor"),
                json!({"id":"r", "error":"", "data":data(b"kept")}),
                "unrelated data survives both writers"
            );
            let whole = json!({"id":"r", "error":"", "data":data(&vec![7; MAX_FILE_BYTES])});
            assert_eq!(f.read("second"), whole, "second complete file is retained");
            assert_eq!(
                f.read("first"),
                if kill { failed("not_found") } else { whole },
                "first file is complete when committed and absent when killed before publication"
            );
            assert!(
                !f.root().join(DOCUMENT_TEMP).exists(),
                "the surviving writer replaces the abandoned temp safely"
            );
        }
    }

    #[test]
    fn a_lock_link_or_nonregular_lock_is_refused() {
        let f = Fixture::new();
        assert_eq!(f.write("f", b"old", "create"), ok());
        let original = fs::read(f.root().join(DOCUMENT)).expect("original tree");
        let lock = f.root().join(DOCUMENT_LOCK);
        fs::remove_file(&lock).expect("remove unlocked sentinel for hostile fixture");
        let target = f.data_dir().join("target");
        fs::write(&target, b"untouched").expect("external target");
        symlink(&target, &lock).expect("hostile lock symlink");
        assert_eq!(
            f.write("f", b"new", "replace"),
            failed("unavailable"),
            "never follow a lock symlink"
        );
        fs::remove_file(&lock).expect("remove symlink");
        fs::hard_link(&target, &lock).expect("hostile hard link");
        assert_eq!(
            f.write("f", b"new", "replace"),
            failed("unavailable"),
            "never share a linked sentinel"
        );
        fs::remove_file(&lock).expect("remove hard link");
        fs::create_dir(&lock).expect("nonregular sentinel");
        assert_eq!(
            f.write("f", b"new", "replace"),
            failed("unavailable"),
            "a directory is not a lock file"
        );
        assert_eq!(
            fs::read(&target).expect("external target after refusal"),
            b"untouched",
            "external bytes remain untouched"
        );
        assert_eq!(
            fs::read(f.root().join(DOCUMENT)).expect("tree after refusal"),
            original,
            "lock failure publishes nothing"
        );
    }

    #[test]
    fn interrupted_writer_process() {
        let Some(dir) = env::var_os("SILENT_STORAGE_INTERRUPTION_DIR") else {
            return;
        };
        let stage = match env::var("SILENT_STORAGE_INTERRUPTION_STAGE")
            .expect("interruption stage")
            .as_str()
        {
            "partial" => DurableWriteStage::TempPartial,
            "written" => DurableWriteStage::TempWritten,
            "synced" => DurableWriteStage::TempSynced,
            "renamed" => DurableWriteStage::Renamed,
            stage => panic!("unknown interruption stage {stage}"),
        };
        let (session, handle) = session_on(Path::new(&dir), PLUGIN);
        state_crypt::on_durable_write_at(stage, || process::exit(86));
        let request = if env::var("SILENT_STORAGE_INTERRUPTION_OP").expect("interrupted operation")
            == "migrate"
        {
            json!({"id": "interrupted", "op": "migrate", "storage": handle, "path": "migrated.json"})
        } else {
            json!({"id": "interrupted", "op": "write", "storage": handle, "path": "f",
                "mode": "replace", "data": BASE64_STANDARD.encode(vec![3; MAX_FILE_BYTES])})
        };
        let answer = send(session, request);
        panic!("writer returned instead of exiting at {stage:?}: {answer}");
    }

    #[test]
    fn process_exit_during_write_and_migration_preserves_complete_data_on_restart() {
        for operation in ["write", "migrate"] {
            for stage in ["partial", "written", "synced", "renamed"] {
                let f = Fixture::new();
                assert_eq!(f.write("f", b"old", "create"), ok());
                let legacy = legacy_blob(f.data_dir(), PLUGIN, b"legacy payload");
                let original_blob = fs::read(&legacy).expect("original legacy envelope");
                let status = Command::new(env::current_exe().expect("test executable"))
                    .args([
                        "--exact",
                        "storage::tests::interrupted_writer_process",
                        "--nocapture",
                    ])
                    .env("SILENT_STORAGE_INTERRUPTION_DIR", f.data_dir())
                    .env("SILENT_STORAGE_INTERRUPTION_STAGE", stage)
                    .env("SILENT_STORAGE_INTERRUPTION_OP", operation)
                    .status()
                    .expect("run separate storage writer");
                assert_eq!(status.code(), Some(86), "{operation} exited during {stage}");
                storage_session_revoke(f.session);
                let (restarted, handle) = session_on(f.data_dir(), PLUGIN);
                let expected = if operation == "write" && stage == "renamed" {
                    vec![3; MAX_FILE_BYTES]
                } else {
                    b"old".to_vec()
                };
                assert_eq!(
                    call(restarted, &handle, "read", json!({"path": "f"})),
                    json!({"id": "r", "error": "", "data": data(&expected)}),
                    "{operation} during {stage}: only complete files after restart"
                );
                if operation == "migrate" {
                    let expected = if stage == "renamed" {
                        json!({"id": "r", "error": "", "data": data(b"legacy payload")})
                    } else {
                        failed("not_found")
                    };
                    assert_eq!(
                        call(restarted, &handle, "read", json!({"path": "migrated.json"})),
                        expected,
                        "migration during {stage}: marker and payload commit together"
                    );
                    let migration = call(
                        restarted,
                        &handle,
                        "migrate",
                        json!({"path": "migrated.json"}),
                    );
                    assert_eq!(
                        migration["error"], "",
                        "migration resumes after {stage}: {migration}"
                    );
                    assert_eq!(
                        migration["migrated"],
                        stage != "renamed",
                        "a committed migration is not repeated"
                    );
                    assert_eq!(
                        call(restarted, &handle, "read", json!({"path": "migrated.json"})),
                        json!({"id": "r", "error": "", "data": data(b"legacy payload")}),
                        "the migrated file is whole"
                    );
                }
                assert_eq!(
                    fs::read(&legacy).expect("retained legacy envelope"),
                    original_blob,
                    "{operation} during {stage}: legacy bytes never changed"
                );
                assert_eq!(
                    call(
                        restarted,
                        &handle,
                        "write",
                        json!({"path": "f", "mode": "replace", "data": data(b"recovered")})
                    ),
                    ok(),
                    "a fresh write replaces any interrupted temp"
                );
                assert!(
                    !f.root().join(DOCUMENT_TEMP).exists(),
                    "no temp after recovery from {stage}"
                );
                storage_session_revoke(restarted);
            }
        }
    }

    #[test]
    fn a_rename_past_the_path_bound_moves_nothing() {
        let f = Fixture::new();
        let name = "n".repeat(MAX_NAME_BYTES);
        // "d/<name> x15/f": 2 + 15 * 256 + 1 bytes, below the bound by 253.
        let deep: Vec<String> = std::iter::once("d".to_string())
            .chain(std::iter::repeat_n(name.clone(), 15))
            .collect();
        for depth in 1..=deep.len() {
            assert_eq!(
                f.call("mkdir", json!({"path": deep[..depth].join("/")})),
                ok()
            );
        }
        let file = format!("{}/f", deep.join("/"));
        assert_eq!(f.write(&file, b"deep", "create"), ok());
        let before = fs::read(f.root().join(DOCUMENT)).expect("tree");
        let to = "r".repeat(MAX_NAME_BYTES);
        assert!(
            format!("{to}{}", &file[1..]).len() > MAX_PATH_BYTES,
            "the moved file's path is past the bound"
        );
        assert_eq!(
            f.call("rename", json!({"path": "d", "to": to})),
            failed("invalid_path")
        );
        assert_eq!(
            fs::read(f.root().join(DOCUMENT)).expect("tree"),
            before,
            "nothing published"
        );
        assert_eq!(
            f.read(&file),
            json!({"id": "r", "error": "", "data": data(b"deep")})
        );
        assert_eq!(f.call("inspect", json!({"path": to})), failed("not_found"));
        assert_eq!(
            f.call("rename", json!({"path": "d", "to": "e"})),
            ok(),
            "a shorter one moves"
        );
    }

    #[test]
    fn revocation_or_cancellation_at_the_commit_boundary_publishes_nothing() {
        for revoke in [true, false] {
            let f = Fixture::new();
            assert_eq!(f.write("f", b"old", "create"), ok());
            let before = fs::read(f.root().join(DOCUMENT)).expect("tree");
            let session = f.session;
            state_crypt::on_durable_write_at(DurableWriteStage::TempSynced, move || {
                if revoke {
                    storage_session_revoke(session);
                } else {
                    storage_request_cancel(session, "r".into());
                }
            });
            let code = if revoke { "revoked" } else { "cancelled" };
            assert_eq!(f.write("f", b"new", "replace"), failed(code));
            assert_eq!(
                fs::read(f.root().join(DOCUMENT)).expect("tree"),
                before,
                "{code}: the synced temp never replaced the tree"
            );
            let (session, handle) = session_on(f.data_dir(), PLUGIN);
            assert_eq!(
                call(session, &handle, "read", json!({"path": "f"})),
                json!({"id": "r", "error": "", "data": data(b"old")}),
                "{code}: the old file"
            );
        }
    }

    #[test]
    fn a_key_change_at_the_commit_boundary_never_publishes_the_old_envelope() {
        let f = Fixture::new();
        assert_eq!(f.write("f", b"old", "create"), ok());
        let before = fs::read(f.root().join(DOCUMENT)).expect("original tree");
        state_crypt::on_durable_write_at(DurableWriteStage::TempSynced, || {
            state_crypt::lock();
            state_crypt::unlock_for_tests();
        });
        assert_eq!(
            f.write("f", b"new", "replace"),
            failed("denied"),
            "even restoring the same key fences an older in-flight write"
        );
        assert_eq!(
            fs::read(f.root().join(DOCUMENT)).expect("tree after refusal"),
            before,
            "a document sealed before the key change never replaces the tree"
        );
        assert_eq!(
            f.read("f"),
            json!({"id": "r", "error": "", "data": data(b"old")}),
            "the next request reads the original complete file"
        );
    }

    #[test]
    fn key_lock_cannot_finish_between_authorization_and_rename() {
        let f = Fixture::new();
        assert_eq!(f.write("f", b"old", "create"), ok());
        let (go_tx, go_rx) = mpsc::channel();
        let (started_tx, started_rx) = mpsc::channel();
        let (done_tx, done_rx) = mpsc::channel();
        let locker = thread::spawn(move || {
            go_rx.recv().expect("start key transition at commit");
            started_tx.send(()).expect("signal key transition attempt");
            state_crypt::lock();
            // The checkpoint drops its receiver once the rename may proceed.
            let _ = done_tx.send(());
        });
        state_crypt::on_durable_write_at(DurableWriteStage::Authorized, move || {
            go_tx
                .send(())
                .expect("attempt key lock after authorization");
            started_rx
                .recv_timeout(Duration::from_secs(5))
                .expect("key transition thread started");
            assert!(
                matches!(
                    done_rx.recv_timeout(Duration::from_millis(100)),
                    Err(mpsc::RecvTimeoutError::Timeout)
                ),
                "key transition is blocked until the authorized rename finishes"
            );
        });
        let answer = f.write("f", b"new", "replace");
        assert!(
            answer == ok() || answer == failed("locked"),
            "publication precedes key lock, response may be fenced: {answer}"
        );
        locker.join().expect("key transition finishes after rename");
        assert!(
            !state_crypt::holds_key(),
            "state is locked after the transition completes"
        );
        state_crypt::unlock_for_tests();
        assert_eq!(
            f.read("f"),
            json!({"id":"r", "error":"", "data":data(b"new")}),
            "the complete authorized publication precedes the key transition"
        );
    }

    // ===== Migration =====
    #[test]
    fn migration_copies_the_legacy_blob_once_without_replacing_or_deleting() {
        let f = Fixture::new();
        assert_eq!(
            f.call("migrate", json!({"path": "state.json"})),
            json!({"id": "r", "error": "", "migrated": false, "reason": "absent"})
        );
        assert!(
            !f.root().join(DOCUMENT).exists(),
            "an absent legacy blob publishes no tree"
        );

        let legacy = legacy_blob(f.data_dir(), PLUGIN, b"\x00legacy");
        let blob = fs::read(&legacy).expect("legacy blob");

        assert_eq!(f.write("taken", b"mine", "create"), ok());
        assert_eq!(
            f.call("migrate", json!({"path": "taken"})),
            failed("already_exists"),
            "a collision is not a completed migration"
        );
        assert_eq!(
            f.read("taken"),
            json!({"id": "r", "error": "", "data": data(b"mine")}),
            "never replaced"
        );
        assert_eq!(
            fs::read(&legacy).expect("legacy kept"),
            blob,
            "the blob is kept"
        );
        assert_eq!(
            f.call("migrate", json!({"path": "missing/state.json"})),
            failed("not_found")
        );

        assert_eq!(
            f.call("migrate", json!({"path": "state.json"})),
            json!({"id": "r", "error": "", "migrated": true})
        );
        assert_eq!(
            f.read("state.json"),
            json!({"id": "r", "error": "", "data": data(b"\x00legacy")})
        );
        assert_eq!(
            f.read("taken"),
            json!({"id": "r", "error": "", "data": data(b"mine")}),
            "both files kept"
        );
        assert_eq!(
            fs::read(&legacy).expect("legacy kept"),
            blob,
            "the blob is kept"
        );

        let completed = json!({"id": "r", "error": "", "migrated": false, "reason": "completed"});
        assert_eq!(f.call("migrate", json!({"path": "state.json"})), completed);
        assert_eq!(
            f.call("migrate", json!({"path": "elsewhere.json"})),
            completed
        );
        assert_eq!(f.call("remove", json!({"path": "state.json"})), ok());
        assert_eq!(
            f.call("migrate", json!({"path": "state.json"})),
            completed,
            "a removed migrated file is never imported again"
        );
        assert_eq!(f.read("state.json"), failed("not_found"));
        let (restarted, handle) = session_on(f.data_dir(), PLUGIN);
        assert_eq!(
            call(restarted, &handle, "migrate", json!({"path": "state.json"})),
            completed,
            "after a restart too"
        );

        let (other, other_handle) = session_on(f.data_dir(), "other");
        assert_eq!(
            call(
                other,
                &other_handle,
                "migrate",
                json!({"path": "state.json"})
            ),
            json!({"id": "r", "error": "", "migrated": false, "reason": "absent"}),
            "a blob belongs to its plugin"
        );
    }

    #[test]
    fn an_unreadable_legacy_blob_is_never_imported() {
        let f = Fixture::new();
        let legacy = legacy_blob(f.data_dir(), PLUGIN, b"x");
        fs::write(&legacy, b"clear").expect("unsealed blob");
        assert_eq!(f.call("migrate", json!({"path": "m"})), failed("corrupt"));
        legacy_blob(f.data_dir(), PLUGIN, &vec![0; MAX_FILE_BYTES + 1]);
        assert_eq!(f.call("migrate", json!({"path": "m"})), failed("too_large"));
        fs::write(&legacy, vec![b'B'; MAX_FILE_BYTES * 3]).expect("oversized blob");
        assert_eq!(
            f.call("migrate", json!({"path": "m"})),
            failed("too_large"),
            "refused before it is read whole"
        );
        assert_eq!(f.read("m"), failed("not_found"));
        assert_eq!(
            f.call("migrate", json!({"path": "m"})),
            failed("too_large"),
            "a failed migration is not completed"
        );
    }

    // ===== Cancellation and revocation =====

    #[test]
    fn a_forgotten_cancellation_no_longer_cancels() {
        let f = Fixture::new();
        storage_request_cancel(f.session, "late".into());
        storage_request_forget(f.session, "late".into());
        assert_eq!(
            send(f.session, json!({"id": "late", "op": "discover"})),
            json!({"id": "late", "error": "", "storages": [{"id": "private", "name": "Private plugin storage"}]})
        );
        storage_request_cancel(f.session, "kept".into());
        storage_request_forget(f.session, "other".into());
        assert_eq!(
            send(f.session, json!({"id": "kept", "op": "discover"})),
            json!({"id": "kept", "error": "cancelled"}),
            "only the id forgotten"
        );
    }

    #[test]
    fn a_request_cancelled_before_it_is_sent_changes_nothing() {
        let f = Fixture::new();
        storage_request_cancel(f.session, "c1".into());
        let write = |id: &str| {
            send(
                f.session,
                json!({"id": id, "op": "write", "storage": f.handle, "path": id, "data": "", "mode": "create"}),
            )
        };
        assert_eq!(write("c1"), json!({"id": "c1", "error": "cancelled"}));
        assert_eq!(f.read("c1"), failed("not_found"));
        assert_eq!(
            write("c1"),
            json!({"id": "c1", "error": ""}),
            "a cancellation is used once"
        );

        for index in 0..=MAX_OUTSTANDING {
            storage_request_cancel(f.session, format!("k{index}"));
        }
        assert_eq!(
            write("k0"),
            json!({"id": "k0", "error": ""}),
            "the oldest cancellation is forgotten"
        );
        assert_eq!(
            write(&format!("k{MAX_OUTSTANDING}")),
            json!({"id": format!("k{MAX_OUTSTANDING}"), "error": "cancelled"})
        );
    }

    #[test]
    fn revocation_and_cancellation_never_wait_and_stop_a_waiting_request() {
        for revoke in [true, false] {
            let f = Fixture::new();
            let turn = OPERATIONS.lock().unwrap_or_else(|e| e.into_inner());
            let (session, handle) = (f.session, f.handle.clone());
            let waiting = thread::spawn(move || {
                call(
                    session,
                    &handle,
                    "write",
                    json!({"path": "late", "data": "", "mode": "create"}),
                )
            });
            wait_running(f.session, 1);
            assert_eq!(
                send(f.session, json!({"id": "r", "op": "discover"})),
                failed("invalid_request"),
                "the same id twice in flight"
            );
            // Both return while another request holds the tree.
            if revoke {
                storage_session_revoke(f.session);
            } else {
                storage_request_cancel(f.session, "r".into());
            }
            drop(turn);
            let answer = waiting.join().expect("waiting request");
            let code = if revoke { "revoked" } else { "cancelled" };
            assert_eq!(answer, failed(code));
            let (session, handle) = session_on(f.data_dir(), PLUGIN);
            assert_eq!(
                call(session, &handle, "read", json!({"path": "late"})),
                failed("not_found"),
                "{code}: nothing written"
            );
        }
    }

    #[test]
    fn a_session_is_busy_past_its_outstanding_bound() {
        let f = Fixture::new();
        let turn = OPERATIONS.lock().unwrap_or_else(|e| e.into_inner());
        let waiting: Vec<_> = (0..MAX_OUTSTANDING)
            .map(|index| {
                let (session, handle) = (f.session, f.handle.clone());
                thread::spawn(move || {
                    send(
                        session,
                        json!({"id": format!("w{index}"), "op": "list", "storage": handle}),
                    )
                })
            })
            .collect();
        wait_running(f.session, MAX_OUTSTANDING);
        assert_eq!(
            send(f.session, json!({"id": "extra", "op": "discover"})),
            json!({"id": "extra", "error": "busy"})
        );
        drop(turn);
        for (index, waiting) in waiting.into_iter().enumerate() {
            assert_eq!(
                waiting.join().expect("waiting request"),
                json!({"id": format!("w{index}"), "error": "", "entries": [], "next": ""})
            );
        }
        assert!(
            lookup(f.session)
                .expect("open session")
                .requests()
                .running
                .is_empty(),
            "answered requests are forgotten"
        );
    }
}
