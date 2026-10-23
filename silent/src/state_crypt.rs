//! The app state, encrypted with BIP138 to a single unlock key (ws-plaude/silent#37).
//!
//! The unlock key is either the xpub a signing device gives at
//! `m/138'/1936225652'/<index>'` (`1936225652` is `shut` in ASCII, the app magic, and
//! `<index>` a hardened index drawn when the state is created), or a random key
//! sealed on disk under a password. With BIP138 the decryption key is that public
//! key itself: the device's is never written anywhere, so the device is needed at
//! every unlock.
//!
//! Nothing about the key is written in clear. `unlock.bip138` is a known text sealed
//! to the key at setup, which proves a key is the right one; for a device key, its
//! BIP138 envelope carries the device path among the derivation paths BIP138 keeps
//! readable, so the app knows which xpub to ask for. A password key is sealed in
//! `unlock.key` with Argon2id and XChaCha20-Poly1305. The check's derivation metadata
//! distinguishes the two modes. Every other state file goes through `read_state` and
//! `write_state`, which refuse to run locked and refuse a file found in clear.

use std::{
    fs,
    path::{Path, PathBuf},
    str::FromStr,
    sync::{Mutex, RwLock, RwLockReadGuard, RwLockWriteGuard},
};

#[cfg(any(unix, windows))]
use std::io::Write;
#[cfg(unix)]
use std::{
    ffi::OsStr,
    os::fd::{AsFd, BorrowedFd},
};

use base64::prelude::BASE64_STANDARD;
use base64::Engine;
use bip138::{Decrypted, EncryptedBackup};
use bitcoin::{
    bip32::{ChildNumber, DerivationPath, Xpub},
    hashes::{sha256, Hash, HashEngine},
    secp256k1::{PublicKey, Secp256k1, SecretKey},
};
use chacha20poly1305::{
    aead::{AeadInPlace, KeyInit},
    XChaCha20Poly1305, XNonce,
};
#[cfg(unix)]
use rustix::{
    fs::{fsync, openat, renameat, unlinkat, AtFlags, Mode, OFlags},
    io::Errno,
};
use zeroize::Zeroizing;

use crate::{
    config::datadir,
    ffi::{StateIndexResult, StateUnlockInfo},
    keys::crypto::Password,
    state_crypt::setup::UnlockState,
};

mod setup;

pub use crate::state_crypt::setup::{
    create_with_device, create_with_password, device_unlock_path, unlock_kind, unlock_with_device,
    unlock_with_password,
};

/// The password key, sealed under the password.
pub const KEY_FILE: &str = "unlock.key";
/// A known text sealed to the key at setup: opening it proves a key is the right one.
pub const CHECK_FILE: &str = "unlock.bip138";
/// The extension of every file sealed to the unlock key.
pub const SEALED_EXTENSION: &str = "bip138";
const CHECK_TEXT: &str = "silent-state-v1";

/// BIP138's purpose.
pub const PURPOSE: u32 = 138;
/// `shut` in ASCII (0x73687574), the app magic.
pub const APP_MAGIC: u32 = 0x7368_7574;

/// Binds the sealed password key to its use.
const KEY_AAD: &[u8] = b"silent-state-key-v1";
const SECRET_DOMAIN: &[u8] = b"silent-state-secret-v1\0";
const SECRET_NONCE_LEN: usize = 24;
const SECRET_TAG_LEN: usize = 16;

/// The unlock key held while the app runs, `None` while locked.
static KEY: Mutex<Option<PublicKey>> = Mutex::new(None);
/// The device path of the unlock key, `None` for a password: written in the clear
/// part of every envelope, so any single file can be opened for debug with the
/// device (ws-plaude/silent#44).
static KEY_PATH: Mutex<Option<DerivationPath>> = Mutex::new(None);
// Take this before key/session locks. Grant/key changes exclude authorization through rename.
static PUBLICATION: RwLock<()> = RwLock::new(());

pub(crate) fn publication_guard() -> RwLockReadGuard<'static, ()> {
    PUBLICATION.read().unwrap_or_else(|e| e.into_inner())
}

pub(crate) fn publication_change_guard() -> RwLockWriteGuard<'static, ()> {
    PUBLICATION.write().unwrap_or_else(|e| e.into_inner())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnlockKind {
    Device,
    Password,
}

/// The path the device unlock key is fetched at.
pub fn device_path(index: u32) -> Result<DerivationPath, String> {
    let child = |i: u32| ChildNumber::from_hardened_idx(i).map_err(|e| e.to_string());
    Ok(DerivationPath::from(vec![
        child(PURPOSE)?,
        child(APP_MAGIC)?,
        child(index)?,
    ]))
}

/// A hardened index below 2^31, drawn when the state is created.
pub fn draw_index() -> Result<u32, String> {
    let mut bytes = [0u8; 4];
    getrandom::getrandom(&mut bytes).map_err(|e| e.to_string())?;
    Ok(u32::from_le_bytes(bytes) & 0x7fff_ffff)
}

/// What bwk writes (each account's stores, its sub-accounts, the header cache) goes
/// through the same key: bwk seals and opens through this cipher while it is set.
#[derive(Debug)]
struct StateCipher;

impl bwk::persist::cipher::Cipher for StateCipher {
    fn seal(&self, plain: &[u8]) -> Result<Vec<u8>, bwk::persist::PersistError> {
        seal(&BASE64_STANDARD.encode(plain)).map_err(bwk::persist::PersistError::Cipher)
    }

    fn extension(&self) -> &str {
        SEALED_EXTENSION
    }

    fn open(&self, sealed: &[u8]) -> Result<Vec<u8>, bwk::persist::PersistError> {
        if !is_sealed(sealed) {
            return Err(bwk::persist::PersistError::Cipher(
                "not encrypted".to_string(),
            ));
        }
        let text = open(sealed).map_err(bwk::persist::PersistError::Cipher)?;
        BASE64_STANDARD
            .decode(text.trim())
            .map_err(|e| bwk::persist::PersistError::Cipher(e.to_string()))
    }
}

fn set_key(key: Option<PublicKey>, path: Option<DerivationPath>) {
    let _publication = publication_change_guard();
    let path = key.and(path);
    let changed = {
        let mut held_key = KEY.lock().unwrap_or_else(|e| e.into_inner());
        let mut held_path = KEY_PATH.lock().unwrap_or_else(|e| e.into_inner());
        let changed = *held_key != key || *held_path != path;
        *held_key = key;
        *held_path = path;
        changed
    };
    if changed {
        crate::storage::key_changed();
    }
    bwk::persist::cipher::set_cipher(key.map(|_| {
        std::sync::Arc::new(StateCipher) as std::sync::Arc<dyn bwk::persist::cipher::Cipher>
    }));
}

fn key() -> Result<PublicKey, String> {
    let key = *KEY.lock().unwrap_or_else(|e| e.into_inner());
    // The crate's own tests run unlocked with the test key.
    #[cfg(test)]
    let key = key.or_else(|| Some(test_key()));
    key.ok_or_else(|| "the app state is locked".to_string())
}

fn test_key() -> PublicKey {
    let secret = SecretKey::from_slice(&[7u8; 32]).expect("valid secret");
    PublicKey::from_secret_key(&Secp256k1::new(), &secret)
}

/// The on-disk name of `name` in the `kind` namespace ("account", "key", "plugin"):
/// a keyed hash, so a directory or file name tells nothing of the account, the key or
/// the plugin it holds. The real name lives in the sealed content. Needs the state
/// unlocked: the key is the unlock key, never stored in clear.
pub fn opaque_name(kind: &str, name: &str) -> Result<String, String> {
    use bitcoin::hashes::{sha256, Hash, HashEngine};
    let key = key()?;
    let mut engine = sha256::Hash::engine();
    engine.input(b"silent-name-v1\0");
    engine.input(&key.serialize());
    engine.input(kind.as_bytes());
    engine.input(b"\0");
    engine.input(name.as_bytes());
    let hash = sha256::Hash::from_engine(engine);
    Ok(hash.to_byte_array()[..16]
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect())
}

pub fn is_unlocked() -> bool {
    key().is_ok()
}

/// Whether the unlock key is held. Unlike `is_unlocked`, the crate tests' fixed key
/// does not count, so a locked state reads as locked in every build.
pub fn holds_key() -> bool {
    KEY.lock().unwrap_or_else(|e| e.into_inner()).is_some()
}

/// Forgets the key: the state cannot be read or written until the next unlock.
pub fn lock() {
    set_key(None, None);
}

fn xpub_key(xpub: &str) -> Result<PublicKey, String> {
    Xpub::from_str(xpub.trim())
        .map(|xpub| xpub.public_key)
        .map_err(|e| format!("invalid xpub: {e}"))
}

fn secret_cipher() -> Result<XChaCha20Poly1305, String> {
    let unlock = Zeroizing::new(key()?.serialize());
    let mut engine = sha256::Hash::engine();
    engine.input(SECRET_DOMAIN);
    engine.input(unlock.as_ref());
    let derived = Zeroizing::new(sha256::Hash::from_engine(engine).to_byte_array());
    Ok(XChaCha20Poly1305::new((&*derived).into()))
}

/// Encrypt raw secret bytes before handing any serialized state to BIP138.
pub fn seal_secret(mut bytes: Zeroizing<Vec<u8>>, aad: &[u8]) -> Result<Vec<u8>, String> {
    let cipher = secret_cipher()?;
    let mut nonce = [0; SECRET_NONCE_LEN];
    getrandom::getrandom(&mut nonce).map_err(|error| error.to_string())?;
    cipher
        .encrypt_in_place(XNonce::from_slice(&nonce), aad, &mut *bytes)
        .map_err(|_| "failed to encrypt state secret")?;
    let mut sealed = nonce.to_vec();
    sealed.extend_from_slice(&bytes);
    Ok(sealed)
}

pub fn open_secret(sealed: &[u8], aad: &[u8]) -> Result<Zeroizing<Vec<u8>>, String> {
    let cipher = secret_cipher()?;
    if sealed.len() < SECRET_NONCE_LEN + SECRET_TAG_LEN {
        return Err("invalid state secret length".into());
    }
    let (nonce, ciphertext) = sealed.split_at(SECRET_NONCE_LEN);
    let mut bytes = Zeroizing::new(ciphertext.to_vec());
    cipher
        .decrypt_in_place(XNonce::from_slice(nonce), aad, &mut *bytes)
        .map_err(|_| "state secret authentication failed")?;
    Ok(bytes)
}

fn seal_with(key: PublicKey, text: &str, path: Option<DerivationPath>) -> Result<Vec<u8>, String> {
    EncryptedBackup::new()
        .set_payload(&text.to_string())
        .map_err(|e| format!("{e:?}"))?
        .set_keys(vec![key])
        .set_derivation_paths(path.into_iter().collect())
        .encrypt()
        .map(|encrypted| encrypted.bytes)
        .map_err(|e| format!("{e:?}"))
}

fn open_with(key: PublicKey, bytes: &[u8]) -> Result<String, String> {
    let decrypted = EncryptedBackup::new()
        .set_encrypted_payload(bytes)
        .map_err(|e| format!("{e:?}"))?
        .set_keys(vec![key])
        .decrypt()
        .map_err(|e| format!("{e:?}"))?;
    decrypted
        .into_iter()
        .find_map(|item| match item {
            Decrypted::String(text) => Some(text),
            _ => None,
        })
        .ok_or_else(|| "no text in the sealed state".to_string())
}

/// Whether `bytes` are a BIP138 envelope, not a file left in clear.
pub fn is_sealed(bytes: &[u8]) -> bool {
    bytes.starts_with(b"BIP138")
}

/// Seals `text` to the unlock key, its device path in the envelope's clear part.
pub fn seal(text: &str) -> Result<Vec<u8>, String> {
    let path = KEY_PATH.lock().unwrap_or_else(|e| e.into_inner()).clone();
    seal_with(key()?, text, path)
}

/// Opens bytes sealed by `seal`.
pub fn open(bytes: &[u8]) -> Result<String, String> {
    open_with(key()?, bytes)
}

/// Reads the state file at `path`: `None` when there is none, an error when it is
/// in clear, sealed to another key, or the state is locked.
pub fn read_state(path: &Path) -> Result<Option<String>, String> {
    if !path.exists() {
        return Ok(None);
    }
    let bytes = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if !is_sealed(&bytes) {
        return Err(format!("{} is not encrypted", path.display()));
    }
    open(&bytes)
        .map(Some)
        .map_err(|e| format!("{}: {e}", path.display()))
}

/// Writes `text` sealed to the unlock key at `path`, replacing any file there.
pub fn write_state(path: &Path, text: &str) -> Result<(), String> {
    let bytes = seal(text)?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    }
    let temp = path.with_extension("tmp");
    fs::write(&temp, &bytes).map_err(|e| format!("{}: {e}", temp.display()))?;
    fs::rename(&temp, path).map_err(|e| format!("{}: {e}", path.display()))
}

/// Syncs the replacement before rename; errors after replacement may leave it installed.
pub(crate) fn write_state_durable(path: &Path, text: &str) -> Result<(), String> {
    let bytes = seal(text)?;
    write_sealed_durable(path, &bytes).map_err(|e| format!("{}: {e}", path.display()))
}

#[cfg(unix)]
fn write_sealed_durable(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| std::io::Error::other("state path has no parent"))?;
    let parent = fs::canonicalize(parent)?;
    let directory = fs::File::open(&parent)?;
    // An account directory may have just been created; persist its ancestor links too.
    for ancestor in parent.ancestors().skip(1) {
        fs::File::open(ancestor)?.sync_all()?;
    }
    let name = path
        .file_name()
        .ok_or_else(|| std::io::Error::other("state path has no file name"))?;
    let temp = Path::new(name).with_extension("tmp");
    replace_at(directory.as_fd(), name, temp.as_os_str(), bytes, || {
        Ok::<(), std::io::Error>(())
    })
}

/// Replaces `name` in `directory` with `bytes`, relative to the directory handle and
/// without following a link: writes a newly created private `temp`, syncs it, asks
/// `commit` and only if it passes renames the temp over `name`, then syncs the
/// directory. An error after the rename can leave the complete new file installed; a
/// partial file is never installed. `commit`'s own error is returned as it is.
#[cfg(unix)]
pub(crate) fn replace_at<E: From<std::io::Error>, G>(
    directory: BorrowedFd<'_>,
    name: &OsStr,
    temp: &OsStr,
    bytes: &[u8],
    commit: impl FnOnce() -> Result<G, E>,
) -> Result<(), E> {
    // A temp left by an interrupted write, or planted there, is unlinked, never followed.
    match unlinkat(directory, temp, AtFlags::empty()) {
        Ok(()) | Err(Errno::NOENT) => {}
        Err(error) => return Err(std::io::Error::from(error).into()),
    }
    let created = openat(
        directory,
        temp,
        OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::NOFOLLOW | OFlags::CLOEXEC,
        Mode::RUSR | Mode::WUSR,
    )
    .map_err(std::io::Error::from)?;
    let mut file = fs::File::from(created);
    #[cfg(test)]
    let bytes = {
        let (first, rest) = bytes.split_at(bytes.len() / 2);
        file.write_all(first)?;
        durable_write_checkpoint(DurableWriteStage::TempPartial)?;
        rest
    };
    file.write_all(bytes)?;
    #[cfg(test)]
    durable_write_checkpoint(DurableWriteStage::TempWritten)?;
    file.sync_all()?;
    drop(file);
    #[cfg(test)]
    durable_write_checkpoint(DurableWriteStage::TempSynced)?;
    let guard = commit()?;
    #[cfg(test)]
    durable_write_checkpoint(DurableWriteStage::Authorized)?;
    renameat(directory, temp, directory, name).map_err(std::io::Error::from)?;
    drop(guard);
    #[cfg(test)]
    durable_write_checkpoint(DurableWriteStage::Renamed)?;
    fsync(directory).map_err(std::io::Error::from)?;
    Ok(())
}

#[cfg(windows)]
fn write_sealed_durable(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| std::io::Error::other("state path has no parent"))?;
    let parent = fs::canonicalize(parent)?;
    if !parent.is_dir() {
        return Err(std::io::Error::other("state directory does not exist"));
    }
    let path = parent.join(
        path.file_name()
            .ok_or_else(|| std::io::Error::other("state path has no file name"))?,
    );
    let temp = path.with_extension("tmp");
    // A stale temp is removed, never truncated through a link left in its place.
    match fs::remove_file(&temp) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temp)?;
    file.write_all(bytes)?;
    #[cfg(test)]
    durable_write_checkpoint(DurableWriteStage::TempWritten)?;
    file.sync_all()?;
    drop(file);
    #[cfg(test)]
    durable_write_checkpoint(DurableWriteStage::TempSynced)?;
    replace_write_through(&temp, &path)?;
    #[cfg(test)]
    durable_write_checkpoint(DurableWriteStage::Renamed)?;
    Ok(())
}

#[cfg(windows)]
pub(crate) fn replace_write_through(temp: &Path, path: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;

    #[link(name = "kernel32")]
    extern "system" {
        #[link_name = "MoveFileExW"]
        fn move_file_ex(existing: *const u16, replacement: *const u16, flags: u32) -> i32;
    }
    const MOVEFILE_REPLACE_EXISTING: u32 = 0x1;
    const MOVEFILE_WRITE_THROUGH: u32 = 0x8;
    let mut source: Vec<_> = temp.as_os_str().encode_wide().collect();
    let mut destination: Vec<_> = path.as_os_str().encode_wide().collect();
    if source.contains(&0) || destination.contains(&0) {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "state path contains a NUL",
        ));
    }
    source.push(0);
    destination.push(0);
    // Both paths are live, NUL-terminated buffers; WRITE_THROUGH waits for the move to reach disk.
    let moved = unsafe {
        move_file_ex(
            source.as_ptr(),
            destination.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if moved == 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(not(any(unix, windows)))]
fn write_sealed_durable(_path: &Path, _bytes: &[u8]) -> std::io::Result<()> {
    Err(std::io::Error::new(
        std::io::ErrorKind::Unsupported,
        "durable state replacement is unavailable on this platform",
    ))
}

#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum DurableWriteStage {
    TempPartial,
    TempWritten,
    TempSynced,
    Authorized,
    Renamed,
    Unlinked,
}

#[cfg(test)]
thread_local! {
    static DURABLE_WRITE_FAILURE: std::cell::Cell<Option<DurableWriteStage>> = const { std::cell::Cell::new(None) };
}

#[cfg(test)]
struct DurableWriteHook {
    stage: DurableWriteStage,
    action: Box<dyn FnOnce()>,
}

#[cfg(test)]
thread_local! {
    static DURABLE_WRITE_HOOK: std::cell::RefCell<Option<DurableWriteHook>> = const { std::cell::RefCell::new(None) };
}

#[cfg(test)]
pub(crate) fn fail_durable_write_at(stage: DurableWriteStage) {
    DURABLE_WRITE_FAILURE.with(|failure| failure.set(Some(stage)));
}

/// Runs `hook` once the next durable write of this thread reaches `stage`.
#[cfg(test)]
pub(crate) fn on_durable_write_at(stage: DurableWriteStage, hook: impl FnOnce() + 'static) {
    DURABLE_WRITE_HOOK.with(|slot| {
        *slot.borrow_mut() = Some(DurableWriteHook {
            stage,
            action: Box::new(hook),
        });
    });
}

#[cfg(test)]
pub(crate) fn durable_write_checkpoint(stage: DurableWriteStage) -> std::io::Result<()> {
    let hook = DURABLE_WRITE_HOOK.with(|slot| {
        let mut slot = slot.borrow_mut();
        match slot.as_ref() {
            Some(hook) if hook.stage == stage => slot.take(),
            _ => None,
        }
    });
    if let Some(hook) = hook {
        (hook.action)();
    }
    DURABLE_WRITE_FAILURE.with(|failure| {
        if failure.get() == Some(stage) {
            failure.set(None);
            Err(std::io::Error::other(format!("interrupted at {stage:?}")))
        } else {
            Ok(())
        }
    })
}

/// Checks `key` against the check file sealed at setup, and unlocks with it.
fn unlock_with(key: PublicKey, device_path: Option<DerivationPath>) -> Result<(), String> {
    let path = datadir().join(CHECK_FILE);
    let bytes = fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    match open_with(key, &bytes) {
        Ok(text) if text == CHECK_TEXT => {
            set_key(Some(key), device_path);
            Ok(())
        }
        _ => Err("this key does not open the app state".to_string()),
    }
}

/// A state file left in clear, `None` when every one is sealed. An unencrypted
/// state is never migrated: the user wipes it. Every file under the data directory
/// is checked, the plugins and bwk's `version` files included, but the password key,
/// the check file and the `.lock` sentinels.
pub fn clear_state_file() -> Option<PathBuf> {
    let root = datadir();
    let mut stack = vec![root.clone()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
                continue;
            }
            let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
            let bookkeeping = [KEY_FILE, CHECK_FILE].contains(&name)
                || name.ends_with(".lock")
                || name.ends_with(".tmp");
            if bookkeeping {
                continue;
            }
            match fs::read(&path) {
                Ok(bytes) if !bytes.is_empty() && !is_sealed(&bytes) => return Some(path),
                _ => {}
            }
        }
    }
    None
}

/// Where the single state blob each plugin saved before its file storage lives, kept
/// only to be migrated (`crate::storage`).
pub const PLUGIN_STATE_DIR: &str = "plugin-state";

/// A plugin id as plugins and their data are named by: no path separator, no leading dot.
pub fn valid_plugin_id(plugin_id: &str) -> bool {
    !plugin_id.is_empty()
        && plugin_id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_'))
        && !plugin_id.starts_with('.')
}

/// The file name, in `PLUGIN_STATE_DIR`, of the state blob `plugin_id` saved before
/// its file storage, its content base64 sealed with `write_state`.
pub fn legacy_plugin_state_name(plugin_id: &str) -> Result<String, String> {
    if !valid_plugin_id(plugin_id) {
        return Err(format!("invalid plugin id '{plugin_id}'"));
    }
    Ok(format!(
        "{}.{SEALED_EXTENSION}",
        opaque_name("plugin", plugin_id)?
    ))
}

/// The state blob `plugin_id` saved under `data_dir` before its file storage.
pub fn legacy_plugin_state_path(data_dir: &Path, plugin_id: &str) -> Result<PathBuf, String> {
    Ok(data_dir
        .join(PLUGIN_STATE_DIR)
        .join(legacy_plugin_state_name(plugin_id)?))
}

// ===== Bridge =====

fn error_of(result: Result<(), String>) -> String {
    result.err().unwrap_or_default()
}

pub fn state_unlock_info() -> StateUnlockInfo {
    match setup::unlock_state() {
        Ok(UnlockState::Empty) => StateUnlockInfo {
            exists: false,
            device: false,
            path: String::new(),
            error: String::new(),
        },
        Ok(UnlockState::Password) => StateUnlockInfo {
            exists: true,
            device: false,
            path: String::new(),
            error: String::new(),
        },
        Ok(UnlockState::Device(path)) => StateUnlockInfo {
            exists: true,
            device: true,
            path: format!("m/{path}"),
            error: String::new(),
        },
        Err(error) => StateUnlockInfo {
            exists: false,
            device: false,
            path: String::new(),
            error,
        },
    }
}

pub fn state_clear_file() -> String {
    clear_state_file()
        .map(|path| path.display().to_string())
        .unwrap_or_default()
}

pub fn state_lock() {
    lock();
}

pub fn state_draw_index() -> StateIndexResult {
    index_result(draw_index())
}

fn index_result(drawn: Result<u32, String>) -> StateIndexResult {
    match drawn {
        Ok(index) => StateIndexResult {
            is_ok: true,
            index,
            error: String::new(),
        },
        Err(error) => StateIndexResult {
            is_ok: false,
            index: 0,
            error: format!("no random index for the device key: {error}"),
        },
    }
}

pub fn state_device_path(index: u32) -> String {
    device_path(index)
        .map(|path| format!("m/{path}"))
        .unwrap_or_default()
}

pub fn state_create_with_device(index: u32, xpub: String) -> String {
    error_of(create_with_device(index, &xpub))
}

pub fn state_create_with_password(password: String) -> String {
    error_of(create_with_password(Password::new(password)))
}

pub fn state_unlock_with_device(xpub: String) -> String {
    error_of(unlock_with_device(&xpub))
}

pub fn state_unlock_with_password(password: String) -> String {
    error_of(unlock_with_password(Password::new(password)))
}

/// Unlocks with a fixed, public key, for tests that write state. Never called by
/// the app, and not reachable from C++: a state sealed to it has no protection.
pub fn unlock_for_tests() {
    set_key(Some(test_key()), None);
}

#[cfg(test)]
mod tests {
    use super::*;

    const XPUB: &str = "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa";

    #[test]
    fn the_device_path_carries_the_app_magic() {
        assert_eq!(
            device_path(42).unwrap().to_string(),
            "138'/1936225652'/42'",
            "m/138'/1936225652'/<index>'"
        );
        assert!(draw_index().unwrap() < 0x8000_0000);
    }

    #[test]
    fn a_failed_draw_is_an_error_not_index_zero() {
        let failed = index_result(Err("no entropy".to_string()));
        assert!(!failed.is_ok, "a failed draw is not ok");
        assert!(failed.error.contains("no entropy"));
        let drawn = index_result(Ok(42));
        assert!(drawn.is_ok && drawn.index == 42 && drawn.error.is_empty());
        assert!(state_draw_index().is_ok, "the system source draws");
    }

    #[test]
    fn a_sealed_text_opens_with_its_key_only() {
        let key = xpub_key(XPUB).unwrap();
        let other = PublicKey::from_secret_key(
            &Secp256k1::new(),
            &SecretKey::from_slice(&[9u8; 32]).unwrap(),
        );
        let sealed = seal_with(key, "{\"a\":1}", None).unwrap();
        assert!(is_sealed(&sealed));
        assert!(
            !sealed.windows(5).any(|w| w == b"\"a\":1"),
            "nothing in clear"
        );
        assert_eq!(open_with(key, &sealed).unwrap(), "{\"a\":1}");
        assert!(
            open_with(other, &sealed).is_err(),
            "another key does not open it"
        );
        assert!(!is_sealed(b"{\"a\":1}"));
    }
}
