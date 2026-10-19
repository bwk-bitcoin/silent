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
//! `unlock.key` with Argon2id and XChaCha20-Poly1305: its presence is what tells a
//! password from a device. Every other state file goes through `read_state` and
//! `write_state`, which refuse to run locked and refuse a file found in clear.

use std::fs;
use std::path::{Path, PathBuf};
use std::str::FromStr;
use std::sync::Mutex;

use base64::prelude::BASE64_STANDARD;
use base64::Engine;
use bip138::{Decrypted, EncryptedBackup};
use bitcoin::bip32::{ChildNumber, DerivationPath, Xpub};
use bitcoin::secp256k1::{PublicKey, Secp256k1, SecretKey};
use zeroize::Zeroizing;

use crate::config::datadir;
use crate::ffi::{PluginStateResult, StateIndexResult, StateUnlockInfo};
use crate::keys::crypto::{self, Password};
use crate::keys::record::SecretEnvelope;

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

/// The unlock key held while the app runs, `None` while locked.
static KEY: Mutex<Option<PublicKey>> = Mutex::new(None);
/// The device path of the unlock key, `None` for a password: written in the clear
/// part of every envelope, so any single file can be opened for debug with the
/// device (ws-plaude/silent#44).
static KEY_PATH: Mutex<Option<DerivationPath>> = Mutex::new(None);

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

/// How the state unlocks, `None` before it is created: with a password when its
/// sealed key is on disk, else with a device.
pub fn unlock_kind() -> Option<UnlockKind> {
    if !datadir().join(CHECK_FILE).exists() {
        return None;
    }
    Some(if datadir().join(KEY_FILE).exists() {
        UnlockKind::Password
    } else {
        UnlockKind::Device
    })
}

/// The path to ask the device for its key at: in the check envelope, readable
/// without the key.
pub fn device_unlock_path() -> Result<DerivationPath, String> {
    let path = datadir().join(CHECK_FILE);
    let bytes = fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let metadata = bip138::EncryptedMetadata::from_encrypted_payload(&bytes)
        .map_err(|e| format!("{}: {e:?}", path.display()))?;
    metadata
        .derivation_paths
        .into_iter()
        .next()
        .ok_or_else(|| format!("{} names no device path", path.display()))
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
    *KEY.lock().unwrap_or_else(|e| e.into_inner()) = key;
    *KEY_PATH.lock().unwrap_or_else(|e| e.into_inner()) = key.and(path);
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

/// Forgets the key: the state cannot be read or written until the next unlock.
pub fn lock() {
    set_key(None, None);
}

fn xpub_key(xpub: &str) -> Result<PublicKey, String> {
    Xpub::from_str(xpub.trim())
        .map(|xpub| xpub.public_key)
        .map_err(|e| format!("invalid xpub: {e}"))
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

fn write_clear(path: &Path, bytes: &[u8]) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    }
    fs::write(path, bytes).map_err(|e| format!("{}: {e}", path.display()))
}

/// Creates the state's check file for `key`, the device path in its clear part for
/// a device key, and unlocks with it.
fn create(key: PublicKey, device_path: Option<DerivationPath>) -> Result<(), String> {
    if datadir().join(CHECK_FILE).exists() {
        return Err("the app state already has an unlock key".to_string());
    }
    let check = seal_with(key, CHECK_TEXT, device_path.clone())?;
    write_clear(&datadir().join(CHECK_FILE), &check)?;
    set_key(Some(key), device_path);
    Ok(())
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

/// Creates the state unlocked by the device whose xpub at `device_path(index)` is
/// `xpub`. The xpub is never written.
pub fn create_with_device(index: u32, xpub: &str) -> Result<(), String> {
    create(xpub_key(xpub)?, Some(device_path(index)?))
}

/// Creates the state unlocked by `password`: a random key, sealed under it.
/// Blocks on Argon2id: call it off the GUI thread.
pub fn create_with_password(password: Password) -> Result<(), String> {
    let mut secret = Zeroizing::new([0u8; 32]);
    let secret_key = loop {
        getrandom::getrandom(secret.as_mut()).map_err(|e| e.to_string())?;
        if let Ok(secret_key) = SecretKey::from_slice(secret.as_ref()) {
            break secret_key;
        }
    };
    let key = PublicKey::from_secret_key(&Secp256k1::new(), &secret_key);
    let envelope =
        crypto::seal_with_aad(&key.serialize(), KEY_AAD, password).map_err(|e| e.to_string())?;
    let sealed = serde_json::to_string_pretty(&envelope).map_err(|e| e.to_string())?;
    write_clear(&datadir().join(KEY_FILE), sealed.as_bytes())?;
    create(key, None)
}

/// Unlocks with the device's xpub at `device_unlock_path()`.
pub fn unlock_with_device(xpub: &str) -> Result<(), String> {
    unlock_with(xpub_key(xpub)?, Some(device_unlock_path()?))
}

/// Unlocks with `password`. Blocks on Argon2id: call it off the GUI thread.
pub fn unlock_with_password(password: Password) -> Result<(), String> {
    let path = datadir().join(KEY_FILE);
    let text = fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let envelope: SecretEnvelope =
        serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    let bytes = crypto::open_with_aad(&envelope, KEY_AAD, password).map_err(|e| e.to_string())?;
    let key = PublicKey::from_slice(&bytes).map_err(|e| e.to_string())?;
    unlock_with(key, None)
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

/// Where a plugin's own state lives.
pub const PLUGIN_STATE_DIR: &str = "plugin-state";

fn plugin_state_path(plugin_id: &str) -> Result<PathBuf, String> {
    let valid = !plugin_id.is_empty()
        && plugin_id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_'))
        && !plugin_id.starts_with('.');
    if !valid {
        return Err(format!("invalid plugin id '{plugin_id}'"));
    }
    Ok(datadir().join(PLUGIN_STATE_DIR).join(format!(
        "{}.{SEALED_EXTENSION}",
        opaque_name("plugin", plugin_id)?
    )))
}

/// The bytes `plugin_id` saved, `None` when it never saved anything.
pub fn read_plugin_state(plugin_id: &str) -> Result<Option<Vec<u8>>, String> {
    let Some(text) = read_state(&plugin_state_path(plugin_id)?)? else {
        return Ok(None);
    };
    BASE64_STANDARD
        .decode(text.trim())
        .map(Some)
        .map_err(|e| format!("plugin state of '{plugin_id}': {e}"))
}

/// Saves `data` as the state of `plugin_id`, sealed with the app state key.
pub fn write_plugin_state(plugin_id: &str, data: &[u8]) -> Result<(), String> {
    write_state(
        &plugin_state_path(plugin_id)?,
        &BASE64_STANDARD.encode(data),
    )
}

// ===== Bridge =====

pub fn plugin_state_load(plugin_id: String) -> PluginStateResult {
    match read_plugin_state(&plugin_id) {
        Ok(data) => PluginStateResult {
            is_ok: true,
            exists: data.is_some(),
            data: data.unwrap_or_default(),
            error: String::new(),
        },
        Err(error) => PluginStateResult {
            is_ok: false,
            exists: false,
            data: Vec::new(),
            error,
        },
    }
}

pub fn plugin_state_save(plugin_id: String, data: Vec<u8>) -> String {
    error_of(write_plugin_state(&plugin_id, &data))
}

fn error_of(result: Result<(), String>) -> String {
    result.err().unwrap_or_default()
}

pub fn state_unlock_info() -> StateUnlockInfo {
    let kind = unlock_kind();
    let device = kind == Some(UnlockKind::Device);
    let path = if device {
        device_unlock_path().map(|path| format!("m/{path}"))
    } else {
        Ok(String::new())
    };
    let (path, error) = match path {
        Ok(path) => (path, String::new()),
        Err(error) => (String::new(), error),
    };
    StateUnlockInfo {
        exists: kind.is_some(),
        device,
        path,
        error,
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
