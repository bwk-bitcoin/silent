use std::{
    fs::{self, File, OpenOptions},
    io::{self, Write},
    path::{Path, PathBuf},
};

#[cfg(unix)]
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
#[cfg(windows)]
use std::os::windows::fs::OpenOptionsExt;

use bitcoin::{
    bip32::DerivationPath,
    secp256k1::{PublicKey, Secp256k1, SecretKey},
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use crate::{
    config::datadir,
    keys::{
        crypto::{self, Password},
        record::SecretEnvelope,
    },
    state_crypt::{
        device_path, seal_with, set_key, unlock_with, xpub_key, UnlockKind, CHECK_FILE, CHECK_TEXT,
        KEY_AAD, KEY_FILE,
    },
    storage::lock_file,
};

const SETUP_LOCK: &str = "unlock-setup.lock";
const TRANSACTION: &str = "unlock-setup.tmp";

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum UnlockState {
    Empty,
    Password,
    Device(DerivationPath),
}

// Only encrypted bytes are journalled. The check's publication commits this pair.
#[derive(Serialize, Deserialize)]
#[serde(tag = "mode", deny_unknown_fields)]
enum SetupFiles {
    Password { key: Vec<u8>, check: Vec<u8> },
    Device { check: Vec<u8> },
}

impl SetupFiles {
    fn check(&self) -> &[u8] {
        match self {
            Self::Password { check, .. } | Self::Device { check } => check,
        }
    }

    fn password_key(&self) -> Option<&[u8]> {
        match self {
            Self::Password { key, .. } => Some(key),
            Self::Device { .. } => None,
        }
    }

    fn validate(&self) -> Result<(), String> {
        let metadata = bip138::EncryptedMetadata::from_encrypted_payload(self.check())
            .map_err(|error| format!("invalid setup check: {error:?}"))?;
        match self {
            Self::Password { key, .. } if metadata.derivation_paths.is_empty() => {
                serde_json::from_slice::<SecretEnvelope>(key)
                    .map_err(|error| format!("invalid setup password envelope: {error}"))?;
                Ok(())
            }
            Self::Device { .. } if metadata.derivation_paths.len() == 1 => Ok(()),
            _ => Err("setup transaction and check unlock modes do not match".into()),
        }
    }
}

struct SetupLock {
    root: PathBuf,
    _file: File,
}

impl SetupLock {
    fn open() -> Result<Self, String> {
        let root = datadir();
        let mut builder = fs::DirBuilder::new();
        builder.recursive(true);
        #[cfg(unix)]
        builder.mode(0o700);
        builder
            .create(&root)
            .map_err(|error| format!("{}: {error}", root.display()))?;
        let mut options = OpenOptions::new();
        options.read(true).write(true).create(true).truncate(false);
        #[cfg(unix)]
        options
            .mode(0o600)
            .custom_flags(rustix::fs::OFlags::NOFOLLOW.bits() as i32);
        #[cfg(windows)]
        options.custom_flags(0x0020_0000); // FILE_FLAG_OPEN_REPARSE_POINT
        let file = options
            .open(root.join(SETUP_LOCK))
            .map_err(|error| format!("cannot open setup lock: {error}"))?;
        if !file
            .metadata()
            .map_err(|error| error.to_string())?
            .is_file()
            || fs::symlink_metadata(root.join(SETUP_LOCK))
                .map_err(|error| error.to_string())?
                .file_type()
                .is_symlink()
        {
            return Err("setup lock is not a regular file".into());
        }
        lock_file(&file).map_err(|error| format!("cannot lock app setup: {error:?}"))?;
        // Take the setup lock before PUBLICATION, KEY and storage session locks.
        let root = fs::canonicalize(root).map_err(|error| error.to_string())?;
        let locked = Self { root, _file: file };
        for ancestor in locked.root.ancestors().skip(1) {
            sync_directory(ancestor)?;
        }
        locked.recover()?;
        Ok(locked)
    }

    fn read(&self, name: &str) -> Result<Option<Vec<u8>>, String> {
        let path = self.root.join(name);
        match fs::symlink_metadata(&path) {
            Ok(metadata) if metadata.is_file() => fs::read(&path)
                .map(Some)
                .map_err(|error| format!("{}: {error}", path.display())),
            Ok(_) => Err(format!("{} is not a regular file", path.display())),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(error) => Err(format!("{}: {error}", path.display())),
        }
    }

    fn sync(&self) -> Result<(), String> {
        sync_directory(&self.root)
    }

    fn publish_new(&self, name: &str, bytes: &[u8]) -> Result<(), String> {
        let mut tag = [0; 16];
        getrandom::getrandom(&mut tag).map_err(|error| error.to_string())?;
        let temp = self.root.join(format!(".unlock-{}.tmp", hex::encode(tag)));
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        options.mode(0o600);
        let mut file = options.open(&temp).map_err(|error| error.to_string())?;
        let result = (|| {
            #[cfg(test)]
            let bytes = if name == TRANSACTION {
                let (first, rest) = bytes.split_at(bytes.len() / 2);
                file.write_all(first).map_err(|error| error.to_string())?;
                checkpoint(SetupStage::JournalPartial)?;
                rest
            } else {
                bytes
            };
            file.write_all(bytes).map_err(|error| error.to_string())?;
            file.sync_all().map_err(|error| error.to_string())?;
            #[cfg(test)]
            match name {
                TRANSACTION => checkpoint(SetupStage::JournalSynced)?,
                CHECK_FILE => checkpoint(SetupStage::CheckSynced)?,
                _ => {}
            }
            // A hard link installs complete bytes atomically and never replaces a target.
            fs::hard_link(&temp, self.root.join(name))
                .map_err(|error| format!("publishing {name}: {error}"))?;
            self.sync()
        })();
        drop(file);
        fs::remove_file(&temp).map_err(|error| format!("{}: {error}", temp.display()))?;
        result
    }

    fn recover(&self) -> Result<(), String> {
        let Some(bytes) = self.read(TRANSACTION)? else {
            return Ok(());
        };
        let files: SetupFiles = serde_json::from_slice(&bytes)
            .map_err(|error| format!("cannot recover setup transaction: {error}"))?;
        files.validate()?;
        match self.read(CHECK_FILE)? {
            Some(check) if check == files.check() => {
                if self.read(KEY_FILE)?.as_deref() != files.password_key() {
                    return Err(
                        "committed setup has an inconsistent password key; recovery required"
                            .into(),
                    );
                }
            }
            Some(_) => {
                return Err(
                    "setup transaction conflicts with the existing unlock check; recovery required"
                        .into(),
                )
            }
            None => {
                if let Some(key) = self.read(KEY_FILE)? {
                    if Some(key.as_slice()) != files.password_key() {
                        return Err(
                            "incomplete setup does not own unlock.key; recovery required".into(),
                        );
                    }
                    fs::remove_file(self.root.join(KEY_FILE)).map_err(|error| error.to_string())?;
                    self.sync()?;
                }
            }
        }
        fs::remove_file(self.root.join(TRANSACTION)).map_err(|error| error.to_string())?;
        self.sync()
    }

    fn state(&self) -> Result<UnlockState, String> {
        let key = self.read(KEY_FILE)?;
        let Some(check) = self.read(CHECK_FILE)? else {
            return match key {
                None => Ok(UnlockState::Empty),
                Some(_) => Err("unlock.key has no committed unlock check or owning setup transaction; recovery required".into()),
            };
        };
        let metadata = bip138::EncryptedMetadata::from_encrypted_payload(&check)
            .map_err(|error| format!("invalid unlock check: {error:?}"))?;
        match (metadata.derivation_paths.as_slice(), key) {
            ([], Some(key)) => {
                serde_json::from_slice::<SecretEnvelope>(&key)
                    .map_err(|error| format!("invalid unlock.key: {error}"))?;
                Ok(UnlockState::Password)
            }
            ([path], None) => Ok(UnlockState::Device(path.clone())),
            _ => Err("unlock check and password key are inconsistent; recovery required".into()),
        }
    }

    fn require_empty(&self) -> Result<(), String> {
        match self.state()? {
            UnlockState::Empty => Ok(()),
            _ => Err("the app state already has an unlock key".into()),
        }
    }

    fn publish(&self, files: &SetupFiles) -> Result<(), String> {
        files.validate()?;
        let transaction = serde_json::to_vec(files).map_err(|error| error.to_string())?;
        self.publish_new(TRANSACTION, &transaction)?;
        let result = (|| {
            #[cfg(test)]
            checkpoint(SetupStage::Journal)?;
            if let Some(key) = files.password_key() {
                self.publish_new(KEY_FILE, key)?;
                #[cfg(test)]
                checkpoint(SetupStage::Key)?;
            }
            self.publish_new(CHECK_FILE, files.check())?;
            #[cfg(test)]
            checkpoint(SetupStage::Check)?;
            Ok::<(), String>(())
        })();
        self.recover().map_err(|recovery| match &result {
            Ok(()) => recovery,
            Err(error) => format!("{error}; setup recovery failed: {recovery}"),
        })?;
        result?;
        #[cfg(test)]
        checkpoint(SetupStage::Finished)?;
        Ok(())
    }
}

fn sync_directory(path: &Path) -> Result<(), String> {
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    options.write(true).custom_flags(0x0200_0000); // FILE_FLAG_BACKUP_SEMANTICS
    options
        .open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|error| format!("syncing {}: {error}", path.display()))
}

/// Serializes recovery, classification, setup and unlock across instances.
pub(crate) fn unlock_state() -> Result<UnlockState, String> {
    SetupLock::open()?.state()
}

pub fn unlock_kind() -> Result<Option<UnlockKind>, String> {
    match unlock_state()? {
        UnlockState::Empty => Ok(None),
        UnlockState::Password => Ok(Some(UnlockKind::Password)),
        UnlockState::Device(_) => Ok(Some(UnlockKind::Device)),
    }
}

pub fn device_unlock_path() -> Result<DerivationPath, String> {
    match unlock_state()? {
        UnlockState::Device(path) => Ok(path),
        _ => Err("the state has no device unlock path".into()),
    }
}

pub fn create_with_device(index: u32, xpub: &str) -> Result<(), String> {
    let locked = SetupLock::open()?;
    locked.require_empty()?;
    let key = xpub_key(xpub)?;
    let path = device_path(index)?;
    let files = SetupFiles::Device {
        check: seal_with(key, CHECK_TEXT, Some(path.clone()))?,
    };
    locked.publish(&files)?;
    set_key(Some(key), Some(path));
    Ok(())
}

/// Includes Argon2id and durable publication; call it off the GUI thread.
pub fn create_with_password(password: Password) -> Result<(), String> {
    let locked = SetupLock::open()?;
    locked.require_empty()?;
    let mut secret = Zeroizing::new([0u8; 32]);
    let secret_key = loop {
        getrandom::getrandom(secret.as_mut()).map_err(|error| error.to_string())?;
        if let Ok(key) = SecretKey::from_slice(secret.as_ref()) {
            break key;
        }
    };
    let key = PublicKey::from_secret_key(&Secp256k1::new(), &secret_key);
    let envelope = crypto::seal_with_aad(&key.serialize(), KEY_AAD, password)
        .map_err(|error| error.to_string())?;
    let files = SetupFiles::Password {
        key: serde_json::to_vec_pretty(&envelope).map_err(|error| error.to_string())?,
        check: seal_with(key, CHECK_TEXT, None)?,
    };
    locked.publish(&files)?;
    set_key(Some(key), None);
    Ok(())
}

pub fn unlock_with_device(xpub: &str) -> Result<(), String> {
    let locked = SetupLock::open()?;
    match locked.state()? {
        UnlockState::Device(path) => unlock_with(xpub_key(xpub)?, Some(path)),
        _ => Err("the state does not use a device unlock key".into()),
    }
}

pub fn unlock_with_password(password: Password) -> Result<(), String> {
    let locked = SetupLock::open()?;
    if locked.state()? != UnlockState::Password {
        return Err("the state does not use a password unlock key".into());
    }
    let key = locked
        .read(KEY_FILE)?
        .ok_or_else(|| "the password key is missing".to_string())?;
    let envelope: SecretEnvelope =
        serde_json::from_slice(&key).map_err(|error| error.to_string())?;
    let bytes =
        crypto::open_with_aad(&envelope, KEY_AAD, password).map_err(|error| error.to_string())?;
    let key = PublicKey::from_slice(&bytes).map_err(|error| error.to_string())?;
    unlock_with(key, None)
}

#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SetupStage {
    JournalPartial,
    JournalSynced,
    Journal,
    Key,
    CheckSynced,
    Check,
    Finished,
}

#[cfg(test)]
fn checkpoint(stage: SetupStage) -> Result<(), String> {
    TEST_HOOK.with(|hook| match hook.borrow_mut().as_mut() {
        Some(hook) => hook(stage),
        None => Ok(()),
    })
}

#[cfg(test)]
type SetupHook = Box<dyn FnMut(SetupStage) -> Result<(), String>>;

#[cfg(test)]
thread_local! {
    static TEST_HOOK: std::cell::RefCell<Option<SetupHook>> = const { std::cell::RefCell::new(None) };
}

#[cfg(test)]
#[path = "setup_tests.rs"]
mod tests;
