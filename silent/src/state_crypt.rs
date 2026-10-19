//! The app state, encrypted with BIP138 to a single unlock key (ws-plaude/silent#37).
//!
//! The unlock key is either the xpub a signing device gives at
//! `m/138'/1936225652'/<index>'` (`1936225652` is `shut` in ASCII, the app magic, and
//! `<index>` a hardened index drawn when the state is created), or a random key
//! sealed on disk under a password. With BIP138 the decryption key is that public
//! key itself: the device's is never written anywhere, so the device is needed at
//! every unlock.
//!
//! What stays in clear: `unlock.json` (the key's kind, the device fingerprint and
//! alias, the index) and, for a password, `unlock.key` (the key sealed with Argon2id
//! and XChaCha20-Poly1305). Every other state file goes through `read_state` and
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
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use crate::config::datadir;
use crate::ffi::{Network, PluginStateResult, StateUnlockInfo};
use crate::keys::crypto::{self, Password};
use crate::keys::record::SecretEnvelope;

/// The clear file naming the unlock key.
pub const UNLOCK_FILE: &str = "unlock.json";
/// The password key, sealed under the password.
pub const KEY_FILE: &str = "unlock.key";
/// A known text sealed to the key at setup: opening it proves a key is the right one.
const CHECK_FILE: &str = "unlock.check";
const CHECK_TEXT: &str = "silent-state-v1";

/// BIP138's purpose.
pub const PURPOSE: u32 = 138;
/// `shut` in ASCII (0x73687574), the app magic.
pub const APP_MAGIC: u32 = 0x7368_7574;

/// Binds the sealed password key to its use.
const KEY_AAD: &[u8] = b"silent-state-key-v1";

/// The unlock key held while the app runs, `None` while locked.
static KEY: Mutex<Option<PublicKey>> = Mutex::new(None);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum UnlockKind {
    Device,
    Password,
}

/// `unlock.json`: in clear, so the unlock screen names the device to plug and asks
/// it for the right path.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UnlockInfo {
    pub kind: UnlockKind,
    /// The device's master fingerprint, empty for a password.
    #[serde(default)]
    pub fingerprint: String,
    /// The device's alias in the keys, empty for a password.
    #[serde(default)]
    pub alias: String,
    /// The hardened index of the device path, 0 for a password.
    #[serde(default)]
    pub index: u32,
    /// The network the device's app is asked on ("bitcoin", "regtest"...), empty for a
    /// password: the key is the same on every network, the app to open is not.
    #[serde(default)]
    pub network: String,
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

fn unlock_path() -> PathBuf {
    datadir().join(UNLOCK_FILE)
}

/// The unlock key's description, `None` before the state is created.
pub fn unlock_info() -> Result<Option<UnlockInfo>, String> {
    let path = unlock_path();
    if !path.exists() {
        return Ok(None);
    }
    let text = fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    serde_json::from_str(&text)
        .map(Some)
        .map_err(|e| format!("{}: {e}", path.display()))
}

fn set_key(key: Option<PublicKey>) {
    *KEY.lock().unwrap_or_else(|e| e.into_inner()) = key;
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

pub fn is_unlocked() -> bool {
    key().is_ok()
}

/// Forgets the key: the state cannot be read or written until the next unlock.
pub fn lock() {
    set_key(None);
}

fn xpub_key(xpub: &str) -> Result<PublicKey, String> {
    Xpub::from_str(xpub.trim())
        .map(|xpub| xpub.public_key)
        .map_err(|e| format!("invalid xpub: {e}"))
}

fn seal_with(key: PublicKey, text: &str) -> Result<Vec<u8>, String> {
    EncryptedBackup::new()
        .set_payload(&text.to_string())
        .map_err(|e| format!("{e:?}"))?
        .set_keys(vec![key])
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

/// Seals `text` to the unlock key.
pub fn seal(text: &str) -> Result<Vec<u8>, String> {
    seal_with(key()?, text)
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

/// Creates the state's unlock files for `key` and unlocks with it.
fn create(info: &UnlockInfo, key: PublicKey) -> Result<(), String> {
    if unlock_path().exists() {
        return Err("the app state already has an unlock key".to_string());
    }
    let check = seal_with(key, CHECK_TEXT)?;
    write_clear(&datadir().join(CHECK_FILE), &check)?;
    let text = serde_json::to_string_pretty(info).map_err(|e| e.to_string())?;
    write_clear(&unlock_path(), text.as_bytes())?;
    set_key(Some(key));
    Ok(())
}

/// Checks `key` against the check file sealed at setup, and unlocks with it.
fn unlock_with(key: PublicKey) -> Result<(), String> {
    let path = datadir().join(CHECK_FILE);
    let bytes = fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    match open_with(key, &bytes) {
        Ok(text) if text == CHECK_TEXT => {
            set_key(Some(key));
            Ok(())
        }
        _ => Err("this key does not open the app state".to_string()),
    }
}

/// Creates the state unlocked by the device `fingerprint`, whose xpub at
/// `device_path(index)` is `xpub`. The xpub is never written.
pub fn create_with_device(
    fingerprint: &str,
    alias: &str,
    index: u32,
    network: &str,
    xpub: &str,
) -> Result<(), String> {
    let info = UnlockInfo {
        kind: UnlockKind::Device,
        fingerprint: fingerprint.to_string(),
        alias: alias.to_string(),
        index,
        network: network.to_string(),
    };
    create(&info, xpub_key(xpub)?)
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
    let info = UnlockInfo {
        kind: UnlockKind::Password,
        fingerprint: String::new(),
        alias: String::new(),
        index: 0,
        network: String::new(),
    };
    create(&info, key)
}

/// Unlocks with the device's xpub at `device_path(index)`.
pub fn unlock_with_device(xpub: &str) -> Result<(), String> {
    unlock_with(xpub_key(xpub)?)
}

/// Unlocks with `password`. Blocks on Argon2id: call it off the GUI thread.
pub fn unlock_with_password(password: Password) -> Result<(), String> {
    let path = datadir().join(KEY_FILE);
    let text = fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let envelope: SecretEnvelope =
        serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    let bytes = crypto::open_with_aad(&envelope, KEY_AAD, password).map_err(|e| e.to_string())?;
    let key = PublicKey::from_slice(&bytes).map_err(|e| e.to_string())?;
    unlock_with(key)
}

/// A state file silent writes left in clear, `None` when every one is sealed. An
/// unencrypted state is never migrated: the user wipes it. Only silent's own files
/// are checked (`app.json`, the hwi config, each account's `config.json`, the key
/// records): the files bwk writes stay in clear until it takes a backend from silent.
pub fn clear_state_file() -> Option<PathBuf> {
    let root = datadir();
    let mut files = vec![
        root.join("app.json"),
        root.join(crate::hwi_config::FILE_NAME),
    ];
    let Ok(entries) = fs::read_dir(&root) else {
        return None;
    };
    for entry in entries.flatten() {
        let dir = entry.path();
        if !dir.is_dir() || dir == root.join("plugins") {
            continue;
        }
        if dir == root.join(PLUGIN_STATE_DIR) {
            for state in fs::read_dir(&dir).into_iter().flatten().flatten() {
                files.push(state.path());
            }
            continue;
        }
        if dir == root.join(crate::keys::KEYS_DIR_NAME) {
            // keys/<network>/<fingerprint>.json
            for network in fs::read_dir(&dir).into_iter().flatten().flatten() {
                for record in fs::read_dir(network.path()).into_iter().flatten().flatten() {
                    let path = record.path();
                    if path.extension().and_then(|e| e.to_str()) == Some("json") {
                        files.push(path);
                    }
                }
            }
            continue;
        }
        files.push(dir.join("config.json"));
    }
    files.into_iter().find(|path| match fs::read(path) {
        Ok(bytes) => !bytes.is_empty() && !is_sealed(&bytes),
        Err(_) => false,
    })
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
    Ok(datadir()
        .join(PLUGIN_STATE_DIR)
        .join(format!("{plugin_id}.json")))
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

fn network_name(network: Network) -> &'static str {
    match network {
        Network::Testnet => "testnet",
        Network::Signet => "signet",
        Network::Regtest => "regtest",
        _ => "bitcoin",
    }
}

fn network_of(name: &str) -> Network {
    match name {
        "testnet" => Network::Testnet,
        "signet" => Network::Signet,
        "regtest" => Network::Regtest,
        _ => Network::Bitcoin,
    }
}

fn error_of(result: Result<(), String>) -> String {
    result.err().unwrap_or_default()
}

pub fn state_unlock_info() -> StateUnlockInfo {
    let none = StateUnlockInfo {
        exists: false,
        device: false,
        fingerprint: String::new(),
        alias: String::new(),
        index: 0,
        network: Network::Bitcoin,
        error: String::new(),
    };
    match unlock_info() {
        Ok(Some(info)) => StateUnlockInfo {
            exists: true,
            device: info.kind == UnlockKind::Device,
            network: network_of(&info.network),
            fingerprint: info.fingerprint,
            alias: info.alias,
            index: info.index,
            error: String::new(),
        },
        Ok(None) => none,
        Err(error) => StateUnlockInfo { error, ..none },
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

pub fn state_draw_index() -> u32 {
    draw_index().unwrap_or(0)
}

pub fn state_device_path(index: u32) -> String {
    device_path(index)
        .map(|path| format!("m/{path}"))
        .unwrap_or_default()
}

pub fn state_create_with_device(
    fingerprint: String,
    alias: String,
    index: u32,
    network: Network,
    xpub: String,
) -> String {
    error_of(create_with_device(
        &fingerprint,
        &alias,
        index,
        network_name(network),
        &xpub,
    ))
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
    set_key(Some(test_key()));
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
    fn a_sealed_text_opens_with_its_key_only() {
        let key = xpub_key(XPUB).unwrap();
        let other = PublicKey::from_secret_key(
            &Secp256k1::new(),
            &SecretKey::from_slice(&[9u8; 32]).unwrap(),
        );
        let sealed = seal_with(key, "{\"a\":1}").unwrap();
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
