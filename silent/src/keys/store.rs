//! Per-network key store: one `KeyRecord` per file at
//! `<root>/<network>/<fingerprint>.json`.
//!
//! Keys live under their own `keys/<network>/` tree rather than beside the account
//! directories so a network name can never collide with an account name. Every
//! read-modify-write takes an exclusive lock on a `.lock` file in the network
//! directory, and writes go to a temp file that is then renamed over the target so a
//! reader never observes a partially written record.

use std::fs::OpenOptions;
use std::io::Read;
use std::path::{Path, PathBuf};

use bitcoin::{bip32::Fingerprint, Network};
use fs2::FileExt;

use crate::keys::{error::Error, network_name, record::KeyRecord};

const LOCK_FILE_NAME: &str = ".lock";
const RECORD_EXTENSION: &str = "json";
const TEMP_EXTENSION: &str = "json.tmp";
const DIR_MODE: u32 = 0o700;
const FILE_MODE: u32 = 0o600;

/// Persists `KeyRecord`s for one network under a caller-supplied root directory.
///
/// The store never resolves `~/.silent/keys` itself: the caller supplies `root`, which
/// keeps this file portable to a future `bwk-keys` crate and testable against a
/// `TempDir`.
pub struct KeyStore {
    root: PathBuf,
    network: Network,
}

impl KeyStore {
    pub fn new(root: PathBuf, network: Network) -> Self {
        Self { root, network }
    }

    pub fn dir(&self) -> Result<PathBuf, Error> {
        Ok(self.root.join(network_name(self.network)?))
    }

    pub fn record_path(&self, fingerprint: Fingerprint) -> Result<PathBuf, Error> {
        Ok(self.dir()?.join(Self::file_name(fingerprint)))
    }

    fn file_name(fingerprint: Fingerprint) -> String {
        format!("{fingerprint}.{RECORD_EXTENSION}")
    }

    /// A missing directory is an empty list, not an error, and reading never creates
    /// the directory.
    pub fn list(&self) -> Result<Vec<KeyRecord>, Error> {
        let dir = self.dir()?;
        let entries = match std::fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(source) => return Err(Error::Io { path: dir, source }),
        };

        let mut records = Vec::new();
        for entry in entries {
            let entry = match entry {
                Ok(entry) => entry,
                Err(source) => return Err(Error::Io { path: dir, source }),
            };
            let path = entry.path();
            if path.file_name().and_then(|name| name.to_str()) == Some(LOCK_FILE_NAME) {
                continue;
            }
            if path.extension().and_then(|ext| ext.to_str()) != Some(RECORD_EXTENSION) {
                continue;
            }
            let Some(stem) = path.file_stem().and_then(|stem| stem.to_str()) else {
                log::error!(
                    "keys::store::list: {} has no valid file stem",
                    path.display()
                );
                continue;
            };
            let stem = stem.to_string();

            match Self::read_record(&path) {
                Ok(record) => match self.validate(record, &path, &stem) {
                    Ok(record) => records.push(record),
                    Err(e) => log::error!("keys::store::list: skipping {}: {e}", path.display()),
                },
                Err(e) => log::error!("keys::store::list: skipping {}: {e}", path.display()),
            }
        }

        records.sort_by_key(|record| record.fingerprint.to_string());
        Ok(records)
    }

    pub fn load(&self, fingerprint: Fingerprint) -> Result<KeyRecord, Error> {
        let path = self.record_path(fingerprint)?;
        if !path.exists() {
            return Err(Error::NotFound(fingerprint));
        }
        let stem = fingerprint.to_string();
        let record = Self::read_record(&path)?;
        self.validate(record, &path, &stem)
    }

    fn read_record(path: &Path) -> Result<KeyRecord, Error> {
        let mut file = std::fs::File::open(path).map_err(|source| Error::Io {
            path: path.to_path_buf(),
            source,
        })?;
        let mut content = String::new();
        file.read_to_string(&mut content)
            .map_err(|source| Error::Io {
                path: path.to_path_buf(),
                source,
            })?;
        serde_json::from_str(&content).map_err(|source| Error::Parse {
            path: path.to_path_buf(),
            source,
        })
    }

    fn validate(&self, record: KeyRecord, path: &Path, stem: &str) -> Result<KeyRecord, Error> {
        record.check_version()?;

        let fingerprint_str = record.fingerprint.to_string();
        if fingerprint_str != stem {
            return match Fingerprint::from_hex(stem) {
                Ok(expected) => Err(Error::FingerprintMismatch {
                    path: path.to_path_buf(),
                    expected,
                    found: record.fingerprint,
                }),
                Err(_) => Err(Error::InvalidFingerprintFileName {
                    path: path.to_path_buf(),
                    stem: stem.to_string(),
                }),
            };
        }

        let expected_network = network_name(self.network)?;
        let found_network = network_name(record.network)?;
        if found_network != expected_network {
            return Err(Error::NetworkMismatch {
                expected: expected_network.to_string(),
                found: found_network.to_string(),
            });
        }

        Ok(record)
    }

    fn ensure_dir(&self) -> Result<PathBuf, Error> {
        let dir = self.dir()?;
        if !dir.exists() {
            #[cfg(unix)]
            {
                use std::fs::DirBuilder;
                use std::os::unix::fs::DirBuilderExt;

                let mut builder = DirBuilder::new();
                builder
                    .mode(DIR_MODE)
                    .recursive(true)
                    .create(&dir)
                    .map_err(|source| Error::Io {
                        path: dir.clone(),
                        source,
                    })?;
            }
            #[cfg(not(unix))]
            {
                std::fs::create_dir_all(&dir).map_err(|source| Error::Io {
                    path: dir.clone(),
                    source,
                })?;
            }
        }
        Ok(dir)
    }

    fn with_lock<T, F>(&self, apply: F) -> Result<T, Error>
    where
        F: FnOnce(&Path) -> Result<T, Error>,
    {
        let dir = self.ensure_dir()?;
        let lock_path = dir.join(LOCK_FILE_NAME);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&lock_path)
            .map_err(|source| Error::Io {
                path: lock_path.clone(),
                source,
            })?;
        file.lock_exclusive().map_err(|source| Error::Io {
            path: lock_path.clone(),
            source,
        })?;

        let result = apply(&dir);
        let _ = FileExt::unlock(&file);
        result
    }

    pub fn save(&self, record: &KeyRecord) -> Result<(), Error> {
        self.with_lock(|dir| self.save_locked(dir, record))
    }

    /// Create-only insert: refuses under the lock if a record already exists on disk
    /// for this fingerprint, so a caller acting on stale in-memory state can never
    /// overwrite a record another process wrote in the meantime.
    pub fn insert(&self, record: &KeyRecord) -> Result<(), Error> {
        self.with_lock(|dir| {
            let path = dir.join(Self::file_name(record.fingerprint));
            if path.exists() {
                return Err(Error::DuplicateKey(record.fingerprint));
            }
            self.save_locked(dir, record)
        })
    }

    fn save_locked(&self, dir: &Path, record: &KeyRecord) -> Result<(), Error> {
        let expected_network = network_name(self.network)?;
        let found_network = network_name(record.network)?;
        if found_network != expected_network {
            return Err(Error::NetworkMismatch {
                expected: expected_network.to_string(),
                found: found_network.to_string(),
            });
        }
        record.check_version()?;

        let final_path = dir.join(Self::file_name(record.fingerprint));
        let temp_path = dir.join(format!("{}.{TEMP_EXTENSION}", record.fingerprint));

        let write_result = self.write_temp(&temp_path, record);
        if let Err(e) = write_result {
            let _ = std::fs::remove_file(&temp_path);
            return Err(e);
        }

        std::fs::rename(&temp_path, &final_path).map_err(|source| {
            let _ = std::fs::remove_file(&temp_path);
            Error::Io {
                path: final_path.clone(),
                source,
            }
        })
    }

    fn write_temp(&self, temp_path: &Path, record: &KeyRecord) -> Result<(), Error> {
        let content = serde_json::to_vec_pretty(record).map_err(|source| Error::Parse {
            path: temp_path.to_path_buf(),
            source,
        })?;

        let mut file = std::fs::File::create(temp_path).map_err(|source| Error::Io {
            path: temp_path.to_path_buf(),
            source,
        })?;

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let permissions = std::fs::Permissions::from_mode(FILE_MODE);
            file.set_permissions(permissions)
                .map_err(|source| Error::Io {
                    path: temp_path.to_path_buf(),
                    source,
                })?;
        }

        std::io::Write::write_all(&mut file, &content).map_err(|source| Error::Io {
            path: temp_path.to_path_buf(),
            source,
        })?;
        file.sync_all().map_err(|source| Error::Io {
            path: temp_path.to_path_buf(),
            source,
        })
    }

    pub fn delete(&self, fingerprint: Fingerprint) -> Result<(), Error> {
        self.with_lock(|dir| {
            let path = dir.join(Self::file_name(fingerprint));
            if !path.exists() {
                return Err(Error::NotFound(fingerprint));
            }
            std::fs::remove_file(&path).map_err(|source| Error::Io { path, source })
        })
    }

    pub fn update<F>(&self, fingerprint: Fingerprint, apply: F) -> Result<KeyRecord, Error>
    where
        F: FnOnce(&mut KeyRecord) -> Result<(), Error>,
    {
        self.with_lock(|dir| {
            let path = dir.join(Self::file_name(fingerprint));
            if !path.exists() {
                return Err(Error::NotFound(fingerprint));
            }
            let stem = fingerprint.to_string();
            let record = Self::read_record(&path)?;
            let mut record = self.validate(record, &path, &stem)?;
            apply(&mut record)?;
            self.save_locked(dir, &record)?;
            Ok(record)
        })
    }
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use bitcoin::bip32::{DerivationPath, Xpub};

    use super::*;
    use crate::keys::record::{DescriptorEntry, DeviceRecord, SecretEnvelope, XpubEntry};
    use crate::keys::KEY_RECORD_VERSION;

    fn fingerprint(value: u32) -> Fingerprint {
        Fingerprint::from(value.to_be_bytes())
    }

    fn test_xpub() -> Xpub {
        let seed = [9u8; 32];
        let xpriv = bitcoin::bip32::Xpriv::new_master(bitcoin::NetworkKind::Test, &seed)
            .expect("valid seed");
        let secp = bitcoin::secp256k1::Secp256k1::new();
        Xpub::from_priv(&secp, &xpriv)
    }

    fn full_record(fingerprint: Fingerprint, network: Network) -> KeyRecord {
        let mut record = KeyRecord::new(fingerprint, network, "alias".to_string());
        record.devices.push(DeviceRecord {
            kind: "hardware".to_string(),
            model: "model".to_string(),
            serial: "serial".to_string(),
            module: "module".to_string(),
        });
        record.xpubs.push(XpubEntry {
            path: DerivationPath::from_str("m/86'/1'/0'").expect("valid path"),
            xpub: test_xpub(),
        });
        record.descriptors.push(DescriptorEntry {
            alias: "descriptor alias".to_string(),
            descriptor: "descriptor string".to_string(),
        });
        record.secret = Some(SecretEnvelope {
            version: 1,
            kdf: "argon2id".to_string(),
            m_cost_kib: 65536,
            t_cost: 3,
            p_cost: 1,
            salt: "salt".to_string(),
            cipher: "chacha20poly1305".to_string(),
            nonce: "nonce".to_string(),
            ciphertext: "ciphertext".to_string(),
        });
        record
    }

    #[test]
    fn list_on_a_missing_directory_is_empty_and_creates_nothing() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);

        let records = store.list().expect("list should succeed");
        assert!(records.is_empty(), "expected empty list, got {records:?}");
        assert!(
            !store.dir().expect("dir path").exists(),
            "list must not create the network directory"
        );
    }

    #[test]
    fn save_then_load_round_trips() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Signet);
        let record = full_record(fingerprint(1), Network::Signet);

        store.save(&record).expect("save should succeed");
        let loaded = store.load(fingerprint(1)).expect("load should succeed");
        assert_eq!(
            loaded, record,
            "loaded record {loaded:?} differs from saved {record:?}"
        );
    }

    #[cfg(unix)]
    #[test]
    fn save_uses_0600_under_a_0700_directory() {
        use std::os::unix::fs::PermissionsExt;

        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let record = full_record(fingerprint(2), Network::Regtest);
        store.save(&record).expect("save should succeed");

        let dir = store.dir().expect("dir path");
        let dir_mode = std::fs::metadata(&dir)
            .expect("dir metadata")
            .permissions()
            .mode()
            & 0o777;
        assert_eq!(
            dir_mode, DIR_MODE,
            "expected dir mode {DIR_MODE:o}, got {dir_mode:o}"
        );

        let file_path = store.record_path(fingerprint(2)).expect("record path");
        let file_mode = std::fs::metadata(&file_path)
            .expect("file metadata")
            .permissions()
            .mode()
            & 0o777;
        assert_eq!(
            file_mode, FILE_MODE,
            "expected file mode {FILE_MODE:o}, got {file_mode:o}"
        );
    }

    #[test]
    fn networks_are_isolated() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let regtest_store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let signet_store = KeyStore::new(tmp.path().to_path_buf(), Network::Signet);

        regtest_store
            .save(&full_record(fingerprint(3), Network::Regtest))
            .expect("save on regtest");
        signet_store
            .save(&full_record(fingerprint(3), Network::Signet))
            .expect("save on signet");

        let regtest_list = regtest_store.list().expect("list regtest");
        let signet_list = signet_store.list().expect("list signet");
        assert_eq!(
            regtest_list.len(),
            1,
            "expected one regtest record, got {regtest_list:?}"
        );
        assert_eq!(
            signet_list.len(),
            1,
            "expected one signet record, got {signet_list:?}"
        );
        assert_ne!(
            regtest_store.dir().expect("regtest dir"),
            signet_store.dir().expect("signet dir"),
            "networks must use different directories"
        );
    }

    #[test]
    fn load_reports_a_missing_record() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);

        let result = store.load(fingerprint(4));
        assert!(
            matches!(result, Err(Error::NotFound(fp)) if fp == fingerprint(4)),
            "expected NotFound, got {result:?}"
        );
    }

    #[test]
    fn a_corrupt_file_is_skipped_by_list_but_errors_on_load() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let valid = full_record(fingerprint(5), Network::Regtest);
        store.save(&valid).expect("save valid record");

        let corrupt_path = store
            .record_path(fingerprint(6))
            .expect("record path for corrupt file");
        std::fs::create_dir_all(corrupt_path.parent().expect("parent dir"))
            .expect("create dir for corrupt file");
        std::fs::write(&corrupt_path, "not json").expect("write corrupt file");

        let listed = store.list().expect("list should succeed");
        assert_eq!(
            listed,
            vec![valid],
            "expected only the valid record in the listing, got {listed:?}"
        );

        let result = store.load(fingerprint(6));
        assert!(
            matches!(result, Err(Error::Parse { .. })),
            "expected Parse error, got {result:?}"
        );
    }

    #[test]
    fn a_newer_version_is_refused_and_not_rewritten() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let mut record = full_record(fingerprint(7), Network::Regtest);
        record.version = KEY_RECORD_VERSION + 1;

        let path = store.record_path(fingerprint(7)).expect("record path");
        std::fs::create_dir_all(path.parent().expect("parent dir")).expect("create dir");
        let bytes = serde_json::to_vec_pretty(&record).expect("serialize record");
        std::fs::write(&path, &bytes).expect("write newer record");

        let load_result = store.load(fingerprint(7));
        assert!(
            matches!(load_result, Err(Error::UnsupportedRecordVersion { .. })),
            "expected UnsupportedRecordVersion on load, got {load_result:?}"
        );

        let save_result = store.save(&record);
        assert!(
            matches!(save_result, Err(Error::UnsupportedRecordVersion { .. })),
            "expected UnsupportedRecordVersion on save, got {save_result:?}"
        );

        let on_disk = std::fs::read(&path).expect("read file back");
        assert_eq!(
            on_disk, bytes,
            "the file on disk must be unchanged after the refused load and save"
        );
    }

    #[test]
    fn a_filename_that_disagrees_with_the_record_is_refused() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let record = full_record(fingerprint(8), Network::Regtest);

        let wrong_path = store.record_path(fingerprint(9)).expect("record path");
        std::fs::create_dir_all(wrong_path.parent().expect("parent dir")).expect("create dir");
        let bytes = serde_json::to_vec_pretty(&record).expect("serialize record");
        std::fs::write(&wrong_path, bytes).expect("write mismatched record");

        let load_result = store.load(fingerprint(9));
        assert!(
            matches!(load_result, Err(Error::FingerprintMismatch { .. })),
            "expected FingerprintMismatch on load, got {load_result:?}"
        );

        let listed = store.list().expect("list should succeed");
        assert!(
            listed.is_empty(),
            "expected the mismatched file to be skipped, got {listed:?}"
        );
    }

    #[test]
    fn insert_refuses_an_existing_record_and_leaves_it_unchanged() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let record = full_record(fingerprint(12), Network::Regtest);
        store.insert(&record).expect("first insert should succeed");

        let path = store.record_path(fingerprint(12)).expect("record path");
        let bytes_before = std::fs::read(&path).expect("read file before");

        let mut duplicate = record.clone();
        duplicate.alias = "other alias".to_string();
        let result = store.insert(&duplicate);
        assert!(
            matches!(result, Err(Error::DuplicateKey(fp)) if fp == fingerprint(12)),
            "expected DuplicateKey on the second insert, got {result:?}"
        );

        let bytes_after = std::fs::read(&path).expect("read file after");
        assert_eq!(
            bytes_before, bytes_after,
            "the file on disk must be unchanged after the refused insert"
        );
    }

    #[test]
    fn update_applies_under_one_lock() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let record = full_record(fingerprint(10), Network::Regtest);
        store.save(&record).expect("save should succeed");

        let updated = store
            .update(fingerprint(10), |record| {
                record.alias = "new alias".to_string();
                Ok(())
            })
            .expect("update should succeed");
        assert_eq!(
            updated.alias, "new alias",
            "update should return the new record"
        );

        let loaded = store.load(fingerprint(10)).expect("load after update");
        assert_eq!(
            loaded.alias, "new alias",
            "a subsequent load should see the updated alias"
        );
    }

    #[test]
    fn delete_removes_the_file_and_reports_a_missing_one() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let record = full_record(fingerprint(11), Network::Regtest);
        store.save(&record).expect("save should succeed");

        store
            .delete(fingerprint(11))
            .expect("first delete should succeed");
        let second = store.delete(fingerprint(11));
        assert!(
            matches!(second, Err(Error::NotFound(fp)) if fp == fingerprint(11)),
            "expected NotFound on second delete, got {second:?}"
        );
    }
}
