use std::{
    collections::BTreeMap,
    fs::File,
    path::PathBuf,
    sync::{Arc, Condvar, Mutex},
};

use bitcoin::hashes::{sha256, Hash};
use bwk::{
    bwk_electrum::persistence::{BackendFactory, BackendIdentity, BackendKind},
    persist::{
        backend::PersistenceBackend,
        config_store::{CallbackConfigStore, ConfigStore},
        PersistError, DB_VERSION,
    },
};
use serde::{de::DeserializeOwned, Deserialize, Serialize};

use crate::{
    config::Config,
    storage::{BuiltinCaller, Client},
};

#[derive(Debug)]
struct Lease {
    file: Mutex<Option<File>>,
    failures: Mutex<BTreeMap<String, String>>,
    backends: Mutex<usize>,
    drained: Condvar,
}

/// Built-in module layout. The host storage engine knows no account schema.
#[derive(Debug, Clone)]
pub(crate) struct AccountStorage {
    client: Client,
    root: String,
}

impl AccountStorage {
    pub(crate) fn open(config: &Config) -> Result<Self, String> {
        Self::named(
            config.data_dir.clone(),
            &config.plugin_id,
            &config.account_name,
        )
    }

    pub(crate) fn named(data_dir: PathBuf, plugin: &str, account: &str) -> Result<Self, String> {
        let caller = match plugin {
            "sp" => BuiltinCaller::Sp,
            "musig" => BuiltinCaller::Musig,
            _ => return Err("unsupported built-in account module".into()),
        };
        Ok(Self {
            client: Client::builtin(data_dir, caller)?,
            root: Self::account_path(account)?,
        })
    }

    fn account_path(account: &str) -> Result<String, String> {
        if account.is_empty() {
            return Err("account ID must be nonempty".into());
        }
        Ok(format!(
            "accounts/{}",
            sha256::Hash::hash(account.as_bytes())
        ))
    }

    pub(crate) fn create(&self) -> Result<(), String> {
        self.client.mkdir("accounts")?;
        self.client.mkdir(&self.root)
    }

    pub(crate) fn read(&self, file: &str) -> Result<Option<Vec<u8>>, String> {
        self.client.read(&format!("{}/{file}", self.root))
    }

    pub(crate) fn write(&self, file: &str, bytes: Vec<u8>, create: bool) -> Result<(), String> {
        self.client
            .write(&format!("{}/{file}", self.root), bytes, create)
    }

    pub(crate) fn remove(&self) -> Result<(), String> {
        self.client.remove(&self.root)
    }

    pub(crate) fn lease(&self) -> Result<File, String> {
        self.client.lease(&self.root)
    }

    pub(crate) fn configs(data_dir: PathBuf, plugin: &str) -> Result<Vec<Config>, String> {
        let storage = Self::named(data_dir.clone(), plugin, "discovery")?;
        let mut configs = Vec::new();
        for path in storage.client.list("accounts")? {
            let Some(bytes) = storage.client.read(&format!("{path}/config.json"))? else {
                continue;
            };
            let mut config: Config = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
            if config.plugin_id != plugin || Self::account_path(&config.account_name)? != path {
                return Err("account config identity does not match its logical directory".into());
            }
            config.data_dir = data_dir.clone();
            configs.push(config);
        }
        Ok(configs)
    }
}

#[derive(Debug)]
pub(crate) struct Factory {
    storage: AccountStorage,
    account: String,
    network: bitcoin::Network,
    lease: Mutex<Option<Arc<Lease>>>,
    closing: Mutex<bool>,
}

impl Factory {
    pub(crate) fn open(config: &Config) -> Result<Arc<Self>, String> {
        let factory = Self::configured(config)?;
        factory.lease()?;
        Ok(factory)
    }

    pub(crate) fn configured(config: &Config) -> Result<Arc<Self>, String> {
        Ok(Arc::new(Self {
            storage: AccountStorage::open(config)?,
            account: config.account_name.clone(),
            network: config.network.into(),
            lease: Mutex::new(None),
            closing: Mutex::new(false),
        }))
    }

    fn lease(&self) -> Result<Arc<Lease>, String> {
        let mut held = self.lease.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(lease) = held.as_ref() {
            return Ok(lease.clone());
        }
        let file = self.storage.lease()?;
        let lease = Arc::new(Lease {
            file: Mutex::new(Some(file)),
            failures: Mutex::new(BTreeMap::new()),
            backends: Mutex::new(0),
            drained: Condvar::new(),
        });
        self.storage.create()?;
        *held = Some(lease.clone());
        Ok(lease)
    }

    pub(crate) fn config_store<C>(&self) -> Arc<dyn ConfigStore<C>>
    where
        C: Serialize + DeserializeOwned + Send + Sync + 'static,
    {
        let write = self.storage.clone();
        let read = self.storage.clone();
        Arc::new(CallbackConfigStore::new(
            move |config: &C| {
                let bytes = serde_json::to_vec_pretty(config)
                    .map_err(|e| PersistError::Serde(e.to_string()))?;
                write
                    .write("backend-config.json", bytes, false)
                    .map_err(PersistError::Io)
            },
            move || {
                read.read("backend-config.json")
                    .map_err(PersistError::Io)?
                    .map(|bytes| {
                        serde_json::from_slice(&bytes)
                            .map_err(|e| PersistError::Serde(e.to_string()))
                    })
                    .transpose()
            },
        ))
    }

    pub(crate) fn failure(&self) -> String {
        self.lease
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .map(|lease| {
                lease
                    .failures
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .values()
                    .cloned()
                    .collect::<Vec<_>>()
                    .join("; ")
            })
            .unwrap_or_default()
    }

    pub(crate) fn begin_shutdown(&self) {
        *self.closing.lock().unwrap_or_else(|e| e.into_inner()) = true;
    }

    pub(crate) fn is_closing(&self) -> bool {
        *self.closing.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub(crate) fn wait_for_backends(&self) {
        let held = self.lease.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(lease) = held.as_ref() {
            let mut count = lease.backends.lock().unwrap_or_else(|e| e.into_inner());
            while *count != 0 {
                count = lease.drained.wait(count).unwrap_or_else(|e| e.into_inner());
            }
            let file = lease.file.lock().unwrap_or_else(|e| e.into_inner()).take();
            drop(file);
        }
    }
}

impl BackendFactory for Factory {
    fn open(
        &self,
        identity: &BackendIdentity,
    ) -> Result<Arc<dyn PersistenceBackend>, PersistError> {
        let closing = self.closing.lock().unwrap_or_else(|e| e.into_inner());
        if *closing {
            return Err(PersistError::Io(
                "account backend factory is closing".into(),
            ));
        }
        if identity.network != self.network {
            return Err(PersistError::Io("backend network mismatch".into()));
        }
        let (name, expected) = match identity.kind {
            BackendKind::Wallet => ("wallet".into(), self.account.clone()),
            BackendKind::SilentPayment => ("sp".into(), self.account.clone()),
            BackendKind::Headers => ("headers".into(), self.account.clone()),
            BackendKind::SubAccount(index) => (
                format!("sub-{index}"),
                format!("{}-sub-{index}", self.account),
            ),
        };
        if identity.account != expected {
            return Err(PersistError::Io("backend account identity mismatch".into()));
        }
        let lease = self.lease().map_err(PersistError::Io)?;
        let path = format!("{}/{}", self.storage.root, name);
        self.storage.client.mkdir(&path).map_err(PersistError::Io)?;
        *lease.backends.lock().unwrap_or_else(|e| e.into_inner()) += 1;
        Ok(Arc::new(WholeFileBackend {
            client: self.storage.client.clone(),
            path,
            lifetime: BackendLifetime(lease),
        }))
    }
}

#[derive(Debug)]
struct WholeFileBackend {
    client: Client,
    path: String,
    // Last: report destruction after the backend's other resources are gone.
    lifetime: BackendLifetime,
}

#[derive(Debug)]
struct BackendLifetime(Arc<Lease>);

impl Drop for BackendLifetime {
    fn drop(&mut self) {
        let mut count = self.0.backends.lock().unwrap_or_else(|e| e.into_inner());
        *count -= 1;
        self.0.drained.notify_all();
    }
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Rows {
    version: u32,
    #[serde(with = "row_bytes")]
    rows: BTreeMap<String, Vec<u8>>,
}

mod row_bytes {
    use std::collections::BTreeMap;

    use base64::{prelude::BASE64_STANDARD, Engine};
    use serde::{de::Error, Deserialize, Deserializer, Serialize, Serializer};

    pub fn serialize<S: Serializer>(
        rows: &BTreeMap<String, Vec<u8>>,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        let encoded: BTreeMap<_, _> = rows
            .iter()
            .map(|(key, bytes)| (key, BASE64_STANDARD.encode(bytes)))
            .collect();
        encoded.serialize(serializer)
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<BTreeMap<String, Vec<u8>>, D::Error> {
        BTreeMap::<String, String>::deserialize(deserializer)?
            .into_iter()
            .map(|(key, value)| {
                BASE64_STANDARD
                    .decode(value)
                    .map(|bytes| (key, bytes))
                    .map_err(D::Error::custom)
            })
            .collect()
    }
}

impl Rows {
    fn decode(bytes: Option<Vec<u8>>) -> Result<Self, PersistError> {
        let rows: Self = match bytes {
            Some(bytes) => {
                serde_json::from_slice(&bytes).map_err(|e| PersistError::Serde(e.to_string()))?
            }
            None => Self {
                version: DB_VERSION,
                rows: BTreeMap::new(),
            },
        };
        if rows.version > DB_VERSION {
            return Err(PersistError::DbVersionTooNew {
                found: rows.version,
                max_supported: DB_VERSION,
            });
        }
        Ok(rows)
    }
}

impl WholeFileBackend {
    fn read(&self, store: &str) -> Result<Rows, PersistError> {
        self.validate_store_name(store)?;
        Rows::decode(
            self.client
                .read(&format!("{}/{store}.json", self.path))
                .map_err(PersistError::Io)?,
        )
    }
}

impl PersistenceBackend for WholeFileBackend {
    fn get_row(&self, store: &str, key: &str) -> Result<Option<Vec<u8>>, PersistError> {
        Ok(self.read(store)?.rows.remove(key))
    }

    fn get_rows(&self, store: &str) -> Result<Vec<(String, Vec<u8>)>, PersistError> {
        Ok(self.read(store)?.rows.into_iter().collect())
    }

    fn put_row(&self, store: &str, key: &str, bytes: &[u8]) -> Result<(), PersistError> {
        self.flush_batch(store, &[(key.into(), bytes.to_vec())], &[])
    }

    fn delete_row(&self, store: &str, key: &str) -> Result<(), PersistError> {
        self.flush_batch(store, &[], &[key.into()])
    }

    fn flush_batch(
        &self,
        store: &str,
        inserts: &[(String, Vec<u8>)],
        removed: &[String],
    ) -> Result<(), PersistError> {
        self.validate_store_name(store)?;
        let path = format!("{}/{store}.json", self.path);
        let result = self.client.update(&path, false, |bytes| {
            let mut rows = Rows::decode(bytes).map_err(|e| e.to_string())?;
            for key in removed {
                rows.rows.remove(key);
            }
            rows.rows.extend(inserts.iter().cloned());
            serde_json::to_vec(&rows).map_err(|e| e.to_string())
        });
        let mut failures = self
            .lifetime
            .0
            .failures
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        match &result {
            Ok(()) => {
                failures.remove(&path);
            }
            Err(error) => {
                failures.insert(path, error.clone());
            }
        }
        result.map_err(PersistError::Io)
    }
}

fn check_owned(plugin: &str, name: &str) -> Result<AccountStorage, String> {
    let storage = AccountStorage::named(crate::config::datadir(), plugin, name)?;
    let bytes = storage.read("config.json")?.ok_or("account not found")?;
    let config: Config = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    if config.plugin_id != plugin || config.account_name != name {
        return Err("account/module identity mismatch".into());
    }
    Ok(storage)
}

pub(crate) fn delete_owned(plugin: &str, name: &str) -> Result<(), String> {
    let storage = check_owned(plugin, name)?;
    let _lease = storage.lease()?;
    storage.remove()
}

pub fn sp_account_delete_check(name: String) -> String {
    check_owned("sp", &name).err().unwrap_or_default()
}

pub fn musig_account_delete_check(name: String) -> String {
    check_owned("musig", &name).err().unwrap_or_default()
}

pub fn delete_sp_account(name: String) -> String {
    delete_owned("sp", &name).err().unwrap_or_default()
}

pub fn delete_musig_account(name: String) -> String {
    delete_owned("musig", &name).err().unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use std::{collections::BTreeMap, fs, sync::Arc};

    use bwk::{
        bwk_electrum::persistence::{BackendFactory, BackendIdentity, BackendKind},
        persist::config_store::ConfigStore,
    };
    use temp_dir::TempDir;

    use crate::{
        account_storage::{AccountStorage, Factory},
        config::Config,
        ffi::Network,
        state_crypt,
        storage::{storage_request, storage_session_open, storage_session_revoke},
    };

    #[test]
    fn real_sp_subaccounts_and_musig_persist_only_in_logical_trees() {
        use std::str::FromStr;

        use bitcoin::{
            bip32::{DerivationPath, Xpriv, Xpub},
            secp256k1::Secp256k1,
        };

        use crate::{
            account::new_account_offline,
            account_storage::Rows,
            config::{segwit_descriptor_for, taproot_descriptor_for},
            musig::new_musig_account_offline,
        };

        state_crypt::unlock_for_tests();
        let dir = TempDir::new().expect("isolated real accounts");
        let secp = Secp256k1::new();
        let master =
            Xpriv::new_master(bitcoin::Network::Regtest, &[31; 32]).expect("fixture master");
        let xpub = |path: &str| {
            let child = master
                .derive_priv(
                    &secp,
                    &DerivationPath::from_str(path).expect("fixture path"),
                )
                .expect("fixture child");
            Xpub::from_priv(&secp, &child).to_string()
        };
        let fingerprint = master.fingerprint(&secp).to_string();
        let mut config = Config::new("real-sp".into(), Network::Regtest,
            "sp(L4rK1yDtCWekvXuE6oXD9jCYfFNV2cWRpVuPLBcCU2z8TrisoyY1,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600)".into(),
            "http://127.0.0.1:1".into(), String::new(), None);
        config.data_dir = dir.path().into();
        config.segwit_descriptor =
            segwit_descriptor_for(fingerprint.clone(), xpub("m/84'/1'/0'"), Network::Regtest);
        config.taproot_descriptor =
            taproot_descriptor_for(fingerprint, xpub("m/86'/1'/0'"), Network::Regtest);
        config.write_file().expect("logical SP config");
        let outpoint = format!("{}:0", "01".repeat(32));
        let mut sp = new_account_offline(&config);
        assert!(sp.is_ok(), "real SP backend: {}", sp.get_error());
        assert_eq!(sp.sub_account_count(), 2, "both real sub-account backends");
        let label = sp.update_coin_label(outpoint.clone(), "SP label".into());
        assert!(label.is_ok, "logical SP label: {}", label.error);
        assert!(
            sp.shutdown_backend().is_empty(),
            "SP fallible shutdown persists"
        );
        drop(sp);
        let storage = AccountStorage::open(&config).expect("SP logical account");
        let labels: Rows = serde_json::from_slice(
            &storage
                .read("sp/labels.json")
                .expect("SP labels")
                .expect("saved SP labels"),
        )
        .expect("label rows");
        assert!(
            labels.rows.values().any(|bytes| bytes == b"\"SP label\""),
            "exact SP serialized label bytes"
        );
        let mut reopened = new_account_offline(&config);
        assert!(
            reopened.is_ok(),
            "SP reload uses the supplied factory: {}",
            reopened.get_error()
        );
        assert!(
            reopened.shutdown_backend().is_empty(),
            "reopened SP shutdown"
        );
        drop(reopened);
        let sp_files = storage.client.list(&storage.root).expect("SP layout");
        assert!(
            sp_files.iter().any(|path| path.ends_with("/sub-0"))
                && sp_files.iter().any(|path| path.ends_with("/sub-1")),
            "separate sub-account identities"
        );
        config.account_name = "real-musig".into();
        config.plugin_id = "musig".into();
        config.descriptor = config.taproot_descriptor.clone();
        config.write_file().expect("logical MuSig config");
        let mut musig = new_musig_account_offline(&config);
        assert!(musig.is_ok(), "real MuSig backend: {}", musig.get_error());
        assert!(
            musig
                .update_coin_label(outpoint, "MuSig label".into())
                .is_ok,
            "MuSig label persisted"
        );
        assert!(
            musig.shutdown_backend().is_empty(),
            "MuSig fallible shutdown persists saved PSBTs too"
        );
        drop(musig);
        let musig_storage = AccountStorage::open(&config).expect("MuSig layout");
        assert!(musig_storage
            .read("saved_psbts.json")
            .expect("logical saved PSBTs")
            .is_some());
        assert!(musig_storage
            .read("backend-config.json")
            .expect("logical backend config")
            .is_some());
        let mut reopened = new_musig_account_offline(&config);
        assert!(
            reopened.is_ok(),
            "MuSig reload uses the supplied factory: {}",
            reopened.get_error()
        );
        assert!(
            reopened.shutdown_backend().is_empty(),
            "reopened MuSig shutdown"
        );
        drop(reopened);
        storage
            .remove()
            .expect("delete only the SP logical subtree");
        assert!(musig_storage
            .read("config.json")
            .expect("other plugin retained")
            .is_some());
        assert_eq!(
            fs::read_dir(dir.path()).expect("physical roots").count(),
            2,
            "only the visible SP and MuSig plugin roots, no filesystem wallet backend"
        );
    }

    /// The configuration of the account `name`, its data in `dir`.
    fn account_config(dir: &TempDir, name: &str) -> Config {
        let mut config = Config::new(
            name.into(),
            Network::Regtest,
            String::new(),
            String::new(),
            String::new(),
            None,
        );
        config.data_dir = dir.path().to_path_buf();
        config
    }

    #[test]
    fn header_stores_past_the_plugin_file_bound_persist_and_reopen() {
        state_crypt::unlock_for_tests();
        let dir = TempDir::new().expect("isolated logical tree");
        // 20000 headers: more than twice the file bound a plugin keeps, once encoded.
        let headers = |seed: u8| -> BTreeMap<String, Vec<u8>> {
            (0..20_000u32)
                .map(|height| {
                    let mut header = vec![seed; bitcoin::block::Header::SIZE];
                    header[..4].copy_from_slice(&height.to_le_bytes());
                    (height.to_string(), header)
                })
                .collect()
        };
        let accounts = [("large-one", headers(1)), ("large-two", headers(2))];
        for (name, rows) in &accounts {
            let config = account_config(&dir, name);
            let factory = Factory::open(&config).expect("account factory");
            let backend = factory
                .open(&BackendIdentity {
                    account: config.account_name.clone(),
                    network: bitcoin::Network::Regtest,
                    kind: BackendKind::Headers,
                })
                .expect("header backend");
            let rows: Vec<(String, Vec<u8>)> = rows.clone().into_iter().collect();
            backend
                .flush_batch(bwk::persist::HEADERS_STORE_KEY, &rows, &[])
                .expect("a header store past the plugin file bound");
            let stored = AccountStorage::open(&config)
                .expect("logical account")
                .read("headers/headers.json")
                .expect("header file")
                .expect("saved headers");
            assert!(
                stored.len() > 2 * crate::storage::MAX_FILE_BYTES,
                "{name}: {} bytes",
                stored.len()
            );
        }
        for (name, rows) in &accounts {
            let config = account_config(&dir, name);
            let factory = Factory::open(&config).expect("reopened account factory");
            let backend = factory
                .open(&BackendIdentity {
                    account: config.account_name.clone(),
                    network: bitcoin::Network::Regtest,
                    kind: BackendKind::Headers,
                })
                .expect("reopened header backend");
            let reopened: BTreeMap<String, Vec<u8>> = backend
                .get_rows(bwk::persist::HEADERS_STORE_KEY)
                .expect("reopened header rows")
                .into_iter()
                .collect();
            assert_eq!(&reopened, rows, "{name}: every header intact");
        }
    }

    #[test]
    fn account_deletion_leaves_no_blob_of_its_files() {
        state_crypt::unlock_for_tests();
        let dir = TempDir::new().expect("isolated logical tree");
        let blobs = || {
            let mut blobs: Vec<String> = fs::read_dir(dir.path().join("sp"))
                .expect("module root")
                .map(|entry| {
                    entry
                        .expect("entry")
                        .file_name()
                        .to_string_lossy()
                        .into_owned()
                })
                .filter(|name| name.ends_with(".blob"))
                .collect();
            blobs.sort();
            blobs
        };
        let kept = AccountStorage::open(&account_config(&dir, "kept")).expect("kept account");
        kept.create().expect("kept directory");
        kept.write("config.json", b"kept".to_vec(), true)
            .expect("kept config");
        let kept_blobs = blobs();
        let deleted =
            AccountStorage::open(&account_config(&dir, "deleted")).expect("deleted account");
        deleted.create().expect("deleted directory");
        deleted
            .write("config.json", b"deleted".to_vec(), true)
            .expect("deleted config");
        deleted
            .write("wallet.json", vec![3; 4096], true)
            .expect("deleted wallet");
        assert_eq!(blobs().len(), 3, "one blob per file");
        let _lease = deleted.lease().expect("account lease");
        deleted.remove().expect("account deletion");
        assert_eq!(blobs(), kept_blobs, "only the kept account's blob is left");
        assert_eq!(
            kept.read("config.json").expect("kept read"),
            Some(b"kept".to_vec())
        );
    }

    #[test]
    fn logical_factory_roundtrip_identity_and_failure_contracts() {
        state_crypt::unlock_for_tests();
        let dir = TempDir::new().expect("isolated logical tree");
        let mut config = Config::new(
            "logical-account".into(),
            Network::Regtest,
            String::new(),
            String::new(),
            String::new(),
            None,
        );
        config.data_dir = dir.path().to_path_buf();
        let storage = AccountStorage::open(&config).expect("built-in caller");
        storage.create().expect("logical account directory");
        storage
            .write(
                "config.json",
                serde_json::to_vec(&config).expect("config payload"),
                true,
            )
            .expect("exclusive config creation");
        assert_eq!(
            storage
                .write("config.json", vec![], true)
                .expect_err("never overwrite account"),
            "already_exists"
        );
        let factory = Factory::open(&config).expect("account factory");
        assert!(
            Factory::open(&config).is_err(),
            "one owner, including closing backends"
        );
        let mut backends = Vec::new();
        for kind in [
            BackendKind::SilentPayment,
            BackendKind::SubAccount(0),
            BackendKind::SubAccount(1),
            BackendKind::Headers,
        ] {
            let account = match kind {
                BackendKind::SubAccount(index) => format!("logical-account-sub-{index}"),
                _ => "logical-account".into(),
            };
            let identity = BackendIdentity {
                account,
                network: bitcoin::Network::Regtest,
                kind,
            };
            let backend = factory.open(&identity).expect("logical backend identity");
            backend
                .put_row("labels", "exact", &[0, 255, 9, 0])
                .expect("opaque byte payload");
            backend
                .flush_batch("labels", &[("second".into(), vec![17])], &["exact".into()])
                .expect("atomic row batch");
            assert_eq!(
                backend.get_rows("labels").expect("read rows"),
                vec![("second".into(), vec![17])]
            );
            assert!(
                backend.get_rows("../../keys").is_err(),
                "store name validation is preserved"
            );
            backends.push((identity, backend));
        }
        {
            let headers = factory
                .open(&BackendIdentity {
                    account: config.account_name.clone(),
                    network: bitcoin::Network::Regtest,
                    kind: BackendKind::Headers,
                })
                .expect("header backend");
            let rows = (0..4032)
                .map(|height| (height.to_string(), vec![0; bitcoin::block::Header::SIZE]))
                .collect::<Vec<_>>();
            headers
                .flush_batch(bwk::persist::HEADERS_STORE_KEY, &rows, &[])
                .expect("two difficulty periods fit one logical file");
            let bytes = storage
                .read("headers/headers.json")
                .expect("header file")
                .expect("saved headers");
            assert!(
                bytes.len() < crate::storage::MAX_FILE_BYTES,
                "opaque headers have compact whole-file encoding"
            );
            assert_eq!(
                headers
                    .get_rows(bwk::persist::HEADERS_STORE_KEY)
                    .expect("exact header rows")
                    .len(),
                4032
            );
        }
        let config_store: Arc<dyn ConfigStore<Config>> = factory.config_store();
        config_store
            .save(&config)
            .expect("logical backend config callback");
        assert_eq!(
            config_store
                .load()
                .expect("load callback")
                .expect("saved config")
                .account_name,
            config.account_name
        );
        drop(config_store);
        drop(factory);
        assert!(
            Factory::open(&config).is_err(),
            "backends retain ownership after factory handle drop"
        );
        let identities = backends
            .iter()
            .map(|(identity, _)| identity.clone())
            .collect::<Vec<_>>();
        drop(backends);
        let factory = Factory::open(&config).expect("reopen after all backend destruction");
        for identity in identities {
            let backend = factory.open(&identity).expect("reopened logical identity");
            assert_eq!(
                backend.get_row("labels", "second").expect("reloaded bytes"),
                Some(vec![17])
            );
        }
        drop(factory);
        let sibling = AccountStorage::named(dir.path().to_path_buf(), "sp", "other-account")
            .expect("sibling");
        sibling.create().expect("sibling directory");
        sibling
            .write("config.json", b"independent".to_vec(), true)
            .expect("sibling payload");
        let visible = dir.path().join("sp");
        assert_eq!(
            fs::read_dir(dir.path()).expect("physical roots").count(),
            1,
            "no opaque filesystem account directories"
        );
        let session = storage_session_open("sp".into(), String::new(), false);
        storage_session_revoke(session);
        assert!(
            storage_request(session, r#"{"id":"r","op":"discover"}"#.into()).contains("revoked")
        );
        storage.remove().expect("logical recursive removal");
        assert!(storage
            .read("config.json")
            .expect("account absent")
            .is_none());
        assert_eq!(
            sibling.read("config.json").expect("sibling retained"),
            Some(b"independent".to_vec())
        );
        let sealed = fs::read(visible.join("tree.bip138")).expect("encrypted tree");
        assert!(
            state_crypt::is_sealed(&sealed),
            "#60 encrypted backing is reused"
        );
        assert!(
            !sealed
                .windows(b"other-account".len())
                .any(|bytes| bytes == b"other-account"),
            "account names are concealed"
        );
        state_crypt::lock();
        assert_eq!(
            sibling.read("config.json").expect_err("locked client"),
            "locked"
        );
        state_crypt::unlock_for_tests();
        assert_eq!(
            sibling
                .read("config.json")
                .expect("same master after unlock"),
            Some(b"independent".to_vec())
        );
    }
}
