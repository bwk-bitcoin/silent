//! In-memory view of every registered key on one network, backed by a `KeyStore`.
//!
//! `KeyManager` owns the records for one network and writes every change through to
//! disk before it updates memory. Everything that can be derived from a record and the
//! live device list, such as whether a key is connected, is computed at call time and
//! never cached.

use std::collections::BTreeMap;
use std::str::FromStr;

use bitcoin::{
    bip32::{DerivationPath, Fingerprint, Xpub},
    secp256k1::{All, Secp256k1},
    Network,
};
use bwk_sp::bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor;

use crate::keys::{
    error::Error,
    network_name,
    record::{DescriptorEntry, DeviceRecord, KeyRecord, RouteKind, XpubEntry},
    store::KeyStore,
};

/// One signing device as currently seen by a hardware backend, for example HWI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LiveDevice {
    /// Stable, serial based where the device provides one.
    pub id: String,
    pub kind: String,
    pub model: String,
    pub serial: String,
    /// Name of the module that produced this device, for example "hwi".
    pub module: String,
    /// None until the device reports one: a locked or unsupported device has none.
    pub fingerprint: Option<Fingerprint>,
}

impl LiveDevice {
    pub fn to_record(&self) -> DeviceRecord {
        DeviceRecord {
            kind: self.kind.clone(),
            model: self.model.clone(),
            serial: self.serial.clone(),
            module: self.module.clone(),
        }
    }
}

pub trait KeyManagerImpl {
    fn network(&self) -> Network;

    fn keys(&self) -> Vec<KeyRecord>;
    fn key(&self, fingerprint: Fingerprint) -> Option<&KeyRecord>;
    fn add_key(&mut self, record: KeyRecord) -> Result<(), Error>;
    fn remove_key(&mut self, fingerprint: Fingerprint, force: bool) -> Result<(), Error>;
    fn set_alias(&mut self, fingerprint: Fingerprint, alias: String) -> Result<(), Error>;

    fn add_route(&mut self, fingerprint: Fingerprint, kind: RouteKind) -> Result<(), Error>;
    fn remove_route(&mut self, fingerprint: Fingerprint, kind: RouteKind) -> Result<(), Error>;
    fn set_route_order(
        &mut self,
        fingerprint: Fingerprint,
        order: Vec<RouteKind>,
    ) -> Result<(), Error>;
    fn best_route(&self, fingerprint: Fingerprint) -> Option<RouteKind>;

    fn record_device(
        &mut self,
        fingerprint: Fingerprint,
        device: DeviceRecord,
    ) -> Result<(), Error>;
    fn set_live_devices(&mut self, live: Vec<LiveDevice>);
    fn live_devices(&self) -> &[LiveDevice];

    fn cache_xpub(
        &mut self,
        fingerprint: Fingerprint,
        path: DerivationPath,
        xpub: Xpub,
    ) -> Result<(), Error>;
    fn xpub(&self, fingerprint: Fingerprint, path: &DerivationPath) -> Option<Xpub>;

    fn set_descriptor_registered(
        &mut self,
        fingerprint: Fingerprint,
        alias: String,
        descriptor: String,
    ) -> Result<(), Error>;
    fn clear_descriptor_registration(
        &mut self,
        fingerprint: Fingerprint,
        descriptor: &str,
    ) -> Result<(), Error>;
    fn registered_descriptors(&self, fingerprint: Fingerprint) -> &[DescriptorEntry];
    fn keys_for_descriptor(&self, descriptor: &str) -> Vec<Fingerprint>;
}

/// Owns the in-memory view of every registered key on one network.
pub struct KeyManager {
    network: Network,
    records: BTreeMap<Fingerprint, KeyRecord>,
    store: KeyStore,
    live: Vec<LiveDevice>,
    secp: Secp256k1<All>,
}

impl KeyManager {
    pub fn new(store: KeyStore, network: Network) -> Result<Self, Error> {
        let records = store
            .list()?
            .into_iter()
            .map(|record| (record.fingerprint, record))
            .collect();
        Ok(Self {
            network,
            records,
            store,
            live: Vec::new(),
            secp: Secp256k1::new(),
        })
    }

    /// Re-reads every record from the store, for when another process wrote one.
    pub fn reload(&mut self) -> Result<(), Error> {
        self.records = self
            .store
            .list()?
            .into_iter()
            .map(|record| (record.fingerprint, record))
            .collect();
        Ok(())
    }

    /// Writes through to the store before committing the change to memory, so memory
    /// never holds a change that failed to reach disk.
    fn mutate<F>(&mut self, fingerprint: Fingerprint, apply: F) -> Result<(), Error>
    where
        F: FnOnce(&mut KeyRecord) -> Result<(), Error>,
    {
        if !self.records.contains_key(&fingerprint) {
            return Err(Error::UnknownKey(fingerprint));
        }
        let saved = self.store.update(fingerprint, apply)?;
        self.records.insert(fingerprint, saved);
        Ok(())
    }

    fn route_is_usable(&self, record: &KeyRecord, kind: RouteKind) -> bool {
        match kind {
            RouteKind::Hot => record.secret.is_some(),
            RouteKind::Usb => self.live_device_for(record.fingerprint).is_some(),
            RouteKind::Qr | RouteKind::SdCard => true,
        }
    }

    /// Derived from the record and the live device list, never stored.
    pub fn is_connected(&self, fingerprint: Fingerprint) -> bool {
        self.live_device_for(fingerprint).is_some()
    }

    /// Derived from the record, never stored.
    pub fn has_hot_secret(&self, fingerprint: Fingerprint) -> bool {
        self.records
            .get(&fingerprint)
            .is_some_and(|record| record.secret.is_some())
    }

    /// The first live device carrying this fingerprint, if any.
    pub fn live_device_for(&self, fingerprint: Fingerprint) -> Option<&LiveDevice> {
        self.live
            .iter()
            .find(|device| device.fingerprint == Some(fingerprint))
    }
}

impl KeyManagerImpl for KeyManager {
    fn network(&self) -> Network {
        self.network
    }

    fn keys(&self) -> Vec<KeyRecord> {
        self.records.values().cloned().collect()
    }

    fn key(&self, fingerprint: Fingerprint) -> Option<&KeyRecord> {
        self.records.get(&fingerprint)
    }

    fn add_key(&mut self, record: KeyRecord) -> Result<(), Error> {
        if record.network != self.network {
            return Err(Error::NetworkMismatch {
                expected: network_name(self.network)?.to_string(),
                found: network_name(record.network)?.to_string(),
            });
        }
        let fingerprint = record.fingerprint;
        match self.store.insert(&record) {
            Ok(()) => {
                self.records.insert(fingerprint, record);
                Ok(())
            }
            Err(Error::DuplicateKey(fp)) => {
                if let Ok(existing) = self.store.load(fp) {
                    self.records.insert(fp, existing);
                }
                Err(Error::DuplicateKey(fp))
            }
            Err(e) => Err(e),
        }
    }

    fn remove_key(&mut self, fingerprint: Fingerprint, force: bool) -> Result<(), Error> {
        let record = self
            .records
            .get(&fingerprint)
            .ok_or(Error::UnknownKey(fingerprint))?;
        let descriptor_count = record.descriptors.len();
        if descriptor_count > 0 && !force {
            return Err(Error::DescriptorsRegistered {
                fingerprint,
                count: descriptor_count,
            });
        }
        self.store.delete(fingerprint)?;
        self.records.remove(&fingerprint);
        Ok(())
    }

    fn set_alias(&mut self, fingerprint: Fingerprint, alias: String) -> Result<(), Error> {
        self.mutate(fingerprint, |record| {
            record.alias = alias;
            Ok(())
        })
    }

    fn add_route(&mut self, fingerprint: Fingerprint, kind: RouteKind) -> Result<(), Error> {
        self.mutate(fingerprint, |record| record.routes.push(kind))
    }

    fn remove_route(&mut self, fingerprint: Fingerprint, kind: RouteKind) -> Result<(), Error> {
        self.mutate(fingerprint, |record| record.routes.remove(kind))
    }

    fn set_route_order(
        &mut self,
        fingerprint: Fingerprint,
        order: Vec<RouteKind>,
    ) -> Result<(), Error> {
        self.mutate(fingerprint, |record| record.routes.set_order(order))
    }

    fn best_route(&self, fingerprint: Fingerprint) -> Option<RouteKind> {
        let record = self.records.get(&fingerprint)?;
        record
            .routes
            .as_slice()
            .iter()
            .copied()
            .find(|kind| self.route_is_usable(record, *kind))
    }

    fn record_device(
        &mut self,
        fingerprint: Fingerprint,
        device: DeviceRecord,
    ) -> Result<(), Error> {
        self.mutate(fingerprint, |record| {
            if record
                .devices
                .iter()
                .any(|existing| existing.same_device(&device))
            {
                return Ok(());
            }
            record.devices.push(device);
            Ok(())
        })
    }

    fn set_live_devices(&mut self, live: Vec<LiveDevice>) {
        self.live = live;
    }

    fn live_devices(&self) -> &[LiveDevice] {
        &self.live
    }

    fn cache_xpub(
        &mut self,
        fingerprint: Fingerprint,
        path: DerivationPath,
        xpub: Xpub,
    ) -> Result<(), Error> {
        self.mutate(fingerprint, |record| {
            if let Some(entry) = record.xpubs.iter().find(|entry| entry.path == path) {
                if entry.xpub == xpub {
                    return Ok(());
                }
                return Err(Error::XpubMismatch {
                    fingerprint,
                    path,
                    cached: Box::new(entry.xpub),
                    found: Box::new(xpub),
                });
            }
            let index = record
                .xpubs
                .iter()
                .position(|entry| entry.path > path)
                .unwrap_or(record.xpubs.len());
            record.xpubs.insert(index, XpubEntry { path, xpub });
            Ok(())
        })
    }

    fn xpub(&self, fingerprint: Fingerprint, path: &DerivationPath) -> Option<Xpub> {
        self.records
            .get(&fingerprint)?
            .xpubs
            .iter()
            .find(|entry| entry.path == *path)
            .map(|entry| entry.xpub)
    }

    fn set_descriptor_registered(
        &mut self,
        fingerprint: Fingerprint,
        alias: String,
        descriptor: String,
    ) -> Result<(), Error> {
        if let Ok(parsed) = SpDescriptor::from_str(&descriptor) {
            match parsed.spend_secret_key(&self.secp) {
                Ok(Some(_)) => return Err(Error::SpSpendDescriptor),
                Ok(None) => {}
                Err(e) => return Err(Error::DescriptorParse(e.to_string())),
            }
        }
        self.mutate(fingerprint, |record| {
            if let Some(entry) = record
                .descriptors
                .iter_mut()
                .find(|entry| entry.descriptor == descriptor)
            {
                if entry.alias != alias {
                    entry.alias = alias;
                }
                return Ok(());
            }
            let index = record
                .descriptors
                .iter()
                .position(|entry| entry.descriptor > descriptor)
                .unwrap_or(record.descriptors.len());
            record
                .descriptors
                .insert(index, DescriptorEntry { alias, descriptor });
            Ok(())
        })
    }

    fn clear_descriptor_registration(
        &mut self,
        fingerprint: Fingerprint,
        descriptor: &str,
    ) -> Result<(), Error> {
        let record = self
            .records
            .get(&fingerprint)
            .ok_or(Error::UnknownKey(fingerprint))?;
        if !record
            .descriptors
            .iter()
            .any(|entry| entry.descriptor == descriptor)
        {
            return Err(Error::DescriptorNotRegistered {
                fingerprint,
                descriptor: descriptor.to_string(),
            });
        }
        self.mutate(fingerprint, |record| {
            record
                .descriptors
                .retain(|entry| entry.descriptor != descriptor);
            Ok(())
        })
    }

    fn registered_descriptors(&self, fingerprint: Fingerprint) -> &[DescriptorEntry] {
        self.records
            .get(&fingerprint)
            .map(|record| record.descriptors.as_slice())
            .unwrap_or(&[])
    }

    fn keys_for_descriptor(&self, descriptor: &str) -> Vec<Fingerprint> {
        self.records
            .iter()
            .filter(|(_, record)| {
                record
                    .descriptors
                    .iter()
                    .any(|entry| entry.descriptor == descriptor)
            })
            .map(|(fingerprint, _)| *fingerprint)
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use super::*;

    fn fingerprint(value: u32) -> Fingerprint {
        Fingerprint::from(value.to_be_bytes())
    }

    fn manager_over(root: &std::path::Path, network: Network) -> KeyManager {
        let store = KeyStore::new(root.to_path_buf(), network);
        KeyManager::new(store, network).expect("build manager")
    }

    fn test_xpub() -> Xpub {
        let seed = [11u8; 32];
        let xpriv = bitcoin::bip32::Xpriv::new_master(bitcoin::NetworkKind::Test, &seed)
            .expect("valid seed");
        let secp = bitcoin::secp256k1::Secp256k1::new();
        Xpub::from_priv(&secp, &xpriv)
    }

    fn other_test_xpub() -> Xpub {
        let seed = [13u8; 32];
        let xpriv = bitcoin::bip32::Xpriv::new_master(bitcoin::NetworkKind::Test, &seed)
            .expect("valid seed");
        let secp = bitcoin::secp256k1::Secp256k1::new();
        Xpub::from_priv(&secp, &xpriv)
    }

    #[test]
    fn new_loads_the_records_already_on_disk() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let first = KeyRecord::new(fingerprint(1), Network::Regtest, "first".to_string());
        let second = KeyRecord::new(fingerprint(2), Network::Regtest, "second".to_string());
        store.save(&first).expect("save first");
        store.save(&second).expect("save second");

        let manager = manager_over(tmp.path(), Network::Regtest);
        let keys = manager.keys();
        assert_eq!(
            keys,
            vec![first, second],
            "expected both records in fingerprint order, got {keys:?}"
        );
    }

    #[test]
    fn add_key_refuses_a_duplicate_fingerprint() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record.clone()).expect("first add succeeds");

        let mut duplicate = record.clone();
        duplicate.alias = "other alias".to_string();
        let result = manager.add_key(duplicate);
        assert!(
            matches!(result, Err(Error::DuplicateKey(fp)) if fp == fingerprint(1)),
            "expected DuplicateKey, got {result:?}"
        );

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let on_disk = store.load(fingerprint(1)).expect("load record");
        assert_eq!(
            on_disk, record,
            "stored file must be unchanged after the refused add, got {on_disk:?}"
        );
    }

    #[test]
    fn add_key_refuses_a_foreign_network() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Signet, "alias".to_string());

        let result = manager.add_key(record);
        assert!(
            matches!(result, Err(Error::NetworkMismatch { .. })),
            "expected NetworkMismatch, got {result:?}"
        );

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let listed = store.list().expect("list");
        assert!(
            listed.is_empty(),
            "expected nothing written for a foreign network record, got {listed:?}"
        );
    }

    #[test]
    fn set_alias_persists() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        manager
            .set_alias(fingerprint(1), "new alias".to_string())
            .expect("set alias");

        let reloaded = manager_over(tmp.path(), Network::Regtest);
        let key = reloaded.key(fingerprint(1)).expect("key present");
        assert_eq!(
            key.alias, "new alias",
            "expected the new alias to persist, got {}",
            key.alias
        );
    }

    #[test]
    fn an_unknown_fingerprint_errors_rather_than_creating_a_record() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let unknown = fingerprint(99);

        let alias_result = manager.set_alias(unknown, "alias".to_string());
        assert!(
            matches!(alias_result, Err(Error::UnknownKey(fp)) if fp == unknown),
            "expected UnknownKey from set_alias, got {alias_result:?}"
        );

        let route_result = manager.add_route(unknown, RouteKind::Usb);
        assert!(
            matches!(route_result, Err(Error::UnknownKey(fp)) if fp == unknown),
            "expected UnknownKey from add_route, got {route_result:?}"
        );

        let remove_result = manager.remove_key(unknown, false);
        assert!(
            matches!(remove_result, Err(Error::UnknownKey(fp)) if fp == unknown),
            "expected UnknownKey from remove_key, got {remove_result:?}"
        );

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let listed = store.list().expect("list");
        assert!(
            listed.is_empty(),
            "expected no file to appear from failed operations, got {listed:?}"
        );
    }

    #[test]
    fn remove_key_refuses_while_descriptors_are_registered() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let mut record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        record.descriptors.push(DescriptorEntry {
            alias: "descriptor alias".to_string(),
            descriptor: "descriptor string".to_string(),
        });
        manager.add_key(record).expect("add key");

        let refused = manager.remove_key(fingerprint(1), false);
        assert!(
            matches!(
                refused,
                Err(Error::DescriptorsRegistered { fingerprint: fp, count: 1 })
                    if fp == fingerprint(1)
            ),
            "expected DescriptorsRegistered with count 1, got {refused:?}"
        );

        manager
            .remove_key(fingerprint(1), true)
            .expect("forced remove succeeds");
        assert!(
            manager.key(fingerprint(1)).is_none(),
            "key should be gone after a forced remove"
        );
    }

    #[test]
    fn routes_persist_and_keep_hot_first() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        manager
            .add_route(fingerprint(1), RouteKind::Usb)
            .expect("add usb");
        manager
            .add_route(fingerprint(1), RouteKind::Qr)
            .expect("add qr");
        manager
            .add_route(fingerprint(1), RouteKind::Hot)
            .expect("add hot");

        let reloaded = manager_over(tmp.path(), Network::Regtest);
        let key = reloaded.key(fingerprint(1)).expect("key present");
        assert_eq!(
            key.routes.as_slice(),
            &[RouteKind::Hot, RouteKind::Usb, RouteKind::Qr],
            "expected hot first, got {:?}",
            key.routes.as_slice()
        );
    }

    #[test]
    fn record_device_dedupes_on_the_four_fields() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        let device = DeviceRecord {
            kind: "hardware".to_string(),
            model: "model".to_string(),
            serial: "serial".to_string(),
            module: "hwi".to_string(),
        };
        manager
            .record_device(fingerprint(1), device.clone())
            .expect("first record");
        manager
            .record_device(fingerprint(1), device.clone())
            .expect("duplicate record is a no-op");

        let key = manager.key(fingerprint(1)).expect("key present");
        assert_eq!(
            key.devices.len(),
            1,
            "expected a duplicate device record to be a no-op, got {:?}",
            key.devices
        );

        let mut different_serial = device.clone();
        different_serial.serial = "other serial".to_string();
        manager
            .record_device(fingerprint(1), different_serial)
            .expect("differing serial records");
        let key = manager.key(fingerprint(1)).expect("key present");
        assert_eq!(
            key.devices.len(),
            2,
            "expected a device differing in serial to add a new entry, got {:?}",
            key.devices
        );
    }

    #[test]
    fn record_device_dedupes_against_disk_not_stale_memory() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager_a = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager_a
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");

        let mut manager_b = manager_over(tmp.path(), Network::Regtest);

        let device = DeviceRecord {
            kind: "hardware".to_string(),
            model: "model".to_string(),
            serial: "serial".to_string(),
            module: "hwi".to_string(),
        };
        manager_a
            .record_device(fp, device.clone())
            .expect("A records the device");
        manager_b
            .record_device(fp, device)
            .expect("B's stale record is a no-op against disk");

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let on_disk = store.load(fp).expect("load record");
        assert_eq!(
            on_disk.devices.len(),
            1,
            "expected no duplicate device on disk, got {:?}",
            on_disk.devices
        );
    }

    #[test]
    fn best_route_skips_an_unusable_route() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");
        manager
            .add_route(fingerprint(1), RouteKind::Usb)
            .expect("add usb");
        manager
            .add_route(fingerprint(1), RouteKind::Qr)
            .expect("add qr");

        let without_device = manager.best_route(fingerprint(1));
        assert_eq!(
            without_device,
            Some(RouteKind::Qr),
            "expected Qr when no live device carries the fingerprint, got {without_device:?}"
        );

        manager.set_live_devices(vec![LiveDevice {
            id: "dev-1".to_string(),
            kind: "hardware".to_string(),
            model: "model".to_string(),
            serial: "serial".to_string(),
            module: "hwi".to_string(),
            fingerprint: Some(fingerprint(1)),
        }]);
        let with_device = manager.best_route(fingerprint(1));
        assert_eq!(
            with_device,
            Some(RouteKind::Usb),
            "expected Usb once a live device carries the fingerprint, got {with_device:?}"
        );

        let hot_only = KeyRecord::new(fingerprint(2), Network::Regtest, "hot only".to_string());
        manager.add_key(hot_only).expect("add hot only key");
        manager
            .add_route(fingerprint(2), RouteKind::Hot)
            .expect("add hot route");
        let no_secret = manager.best_route(fingerprint(2));
        assert_eq!(
            no_secret, None,
            "expected None for a hot route with no secret, got {no_secret:?}"
        );
    }

    #[test]
    fn set_live_devices_persists_nothing() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);

        manager.set_live_devices(vec![LiveDevice {
            id: "dev-1".to_string(),
            kind: "hardware".to_string(),
            model: "model".to_string(),
            serial: "serial".to_string(),
            module: "hwi".to_string(),
            fingerprint: Some(fingerprint(42)),
        }]);

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let listed = store.list().expect("list");
        assert!(
            listed.is_empty(),
            "expected no file for an unregistered live device, got {listed:?}"
        );
    }

    #[test]
    fn cache_xpub_persists_and_reads_back() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        let path = DerivationPath::from_str("m/86'/1'/0'").expect("valid path");
        let xpub = test_xpub();
        manager
            .cache_xpub(fingerprint(1), path.clone(), xpub)
            .expect("cache xpub");

        let reloaded = manager_over(tmp.path(), Network::Regtest);
        let found = reloaded.xpub(fingerprint(1), &path);
        assert_eq!(
            found,
            Some(xpub),
            "expected the cached xpub to persist, got {found:?}"
        );
    }

    #[test]
    fn caching_the_same_xpub_twice_does_not_rewrite_the_file() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        let path = DerivationPath::from_str("m/86'/1'/0'").expect("valid path");
        let xpub = test_xpub();
        manager
            .cache_xpub(fingerprint(1), path.clone(), xpub)
            .expect("first cache");

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let file_path = store.record_path(fingerprint(1)).expect("record path");
        let before = std::fs::read(&file_path).expect("read file before");

        manager
            .cache_xpub(fingerprint(1), path, xpub)
            .expect("second cache is a no-op");

        let after = std::fs::read(&file_path).expect("read file after");
        assert_eq!(
            before, after,
            "expected caching the same xpub twice to leave the file untouched"
        );
    }

    #[test]
    fn a_different_xpub_at_the_same_path_is_refused() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        let path = DerivationPath::from_str("m/86'/1'/0'").expect("valid path");
        let first = test_xpub();
        manager
            .cache_xpub(fingerprint(1), path.clone(), first)
            .expect("first cache");

        let second = other_test_xpub();
        let result = manager.cache_xpub(fingerprint(1), path.clone(), second);
        assert!(
            matches!(
                result,
                Err(Error::XpubMismatch { ref cached, ref found, .. })
                    if **cached == first && **found == second
            ),
            "expected XpubMismatch carrying both values, got {result:?}"
        );

        let still_cached = manager.xpub(fingerprint(1), &path);
        assert_eq!(
            still_cached,
            Some(first),
            "expected the original xpub to remain cached, got {still_cached:?}"
        );
    }

    #[test]
    fn cache_xpub_detects_a_stale_conflict_against_disk() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager_a = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager_a
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");

        let mut manager_b = manager_over(tmp.path(), Network::Regtest);

        let path = DerivationPath::from_str("m/86'/1'/0'").expect("valid path");
        let first = test_xpub();
        manager_a
            .cache_xpub(fp, path.clone(), first)
            .expect("A caches the first xpub");

        let second = other_test_xpub();
        let result = manager_b.cache_xpub(fp, path.clone(), second);
        assert!(
            matches!(
                result,
                Err(Error::XpubMismatch { ref cached, ref found, .. })
                    if **cached == first && **found == second
            ),
            "expected XpubMismatch against the on-disk xpub, got {result:?}"
        );

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let on_disk = store.load(fp).expect("load record");
        assert_eq!(
            on_disk.xpubs,
            vec![XpubEntry { path, xpub: first }],
            "expected only A's xpub on disk, got {:?}",
            on_disk.xpubs
        );
    }

    #[test]
    fn xpub_on_an_unknown_path_is_none() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        let path = DerivationPath::from_str("m/86'/1'/0'").expect("valid path");
        let found = manager.xpub(fingerprint(1), &path);
        assert_eq!(
            found, None,
            "expected no cached xpub for an unqueried path, got {found:?}"
        );
    }

    #[test]
    fn cache_xpub_on_an_unknown_fingerprint_errors() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let unknown = fingerprint(99);
        let path = DerivationPath::from_str("m/86'/1'/0'").expect("valid path");

        let result = manager.cache_xpub(unknown, path, test_xpub());
        assert!(
            matches!(result, Err(Error::UnknownKey(fp)) if fp == unknown),
            "expected UnknownKey, got {result:?}"
        );

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let listed = store.list().expect("list");
        assert!(
            listed.is_empty(),
            "expected no file for a cache on an unknown fingerprint, got {listed:?}"
        );
    }

    #[test]
    fn set_descriptor_registered_persists_and_dedupes() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        manager
            .set_descriptor_registered(
                fingerprint(1),
                "first alias".to_string(),
                "descriptor".to_string(),
            )
            .expect("first register");
        manager
            .set_descriptor_registered(
                fingerprint(1),
                "first alias".to_string(),
                "descriptor".to_string(),
            )
            .expect("duplicate register is a no-op");

        let reloaded = manager_over(tmp.path(), Network::Regtest);
        let descriptors = reloaded.registered_descriptors(fingerprint(1));
        assert_eq!(
            descriptors,
            &[DescriptorEntry {
                alias: "first alias".to_string(),
                descriptor: "descriptor".to_string(),
            }],
            "expected a single entry after registering twice with the same alias, got {descriptors:?}"
        );

        manager
            .set_descriptor_registered(
                fingerprint(1),
                "new alias".to_string(),
                "descriptor".to_string(),
            )
            .expect("re-register with a new alias");
        let descriptors = manager.registered_descriptors(fingerprint(1));
        assert_eq!(
            descriptors,
            &[DescriptorEntry {
                alias: "new alias".to_string(),
                descriptor: "descriptor".to_string(),
            }],
            "expected the alias to update in place, got {descriptors:?}"
        );
    }

    #[test]
    fn set_descriptor_registered_dedupes_against_disk_not_stale_memory() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager_a = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager_a
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");

        let mut manager_b = manager_over(tmp.path(), Network::Regtest);

        manager_a
            .set_descriptor_registered(fp, "alias one".to_string(), "descriptor".to_string())
            .expect("A registers the descriptor");
        manager_b
            .set_descriptor_registered(fp, "alias one".to_string(), "descriptor".to_string())
            .expect("B's stale register is a no-op against disk");

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let on_disk = store.load(fp).expect("load record");
        assert_eq!(
            on_disk.descriptors.len(),
            1,
            "expected no duplicate descriptor on disk, got {:?}",
            on_disk.descriptors
        );
    }

    #[test]
    fn clear_descriptor_registration_removes_the_entry() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");
        manager
            .set_descriptor_registered(
                fingerprint(1),
                "alias".to_string(),
                "descriptor".to_string(),
            )
            .expect("register");

        manager
            .clear_descriptor_registration(fingerprint(1), "descriptor")
            .expect("clear registration");
        let descriptors = manager.registered_descriptors(fingerprint(1));
        assert!(
            descriptors.is_empty(),
            "expected no descriptors after clearing, got {descriptors:?}"
        );

        let second = manager.clear_descriptor_registration(fingerprint(1), "descriptor");
        assert!(
            matches!(
                second,
                Err(Error::DescriptorNotRegistered { fingerprint: fp, ref descriptor })
                    if fp == fingerprint(1) && descriptor == "descriptor"
            ),
            "expected DescriptorNotRegistered on the second clear, got {second:?}"
        );
    }

    #[test]
    fn keys_for_descriptor_finds_every_key() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        manager
            .add_key(KeyRecord::new(
                fingerprint(1),
                Network::Regtest,
                "first".to_string(),
            ))
            .expect("add first key");
        manager
            .add_key(KeyRecord::new(
                fingerprint(2),
                Network::Regtest,
                "second".to_string(),
            ))
            .expect("add second key");

        manager
            .set_descriptor_registered(fingerprint(2), "alias 2".to_string(), "shared".to_string())
            .expect("register on second");
        manager
            .set_descriptor_registered(fingerprint(1), "alias 1".to_string(), "shared".to_string())
            .expect("register on first");

        let found = manager.keys_for_descriptor("shared");
        assert_eq!(
            found,
            vec![fingerprint(1), fingerprint(2)],
            "expected both fingerprints in fingerprint order, got {found:?}"
        );

        let empty = manager.keys_for_descriptor("unregistered");
        assert!(
            empty.is_empty(),
            "expected no keys for an unregistered descriptor, got {empty:?}"
        );
    }

    #[test]
    fn registered_descriptors_on_an_unknown_fingerprint_is_empty() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let manager = manager_over(tmp.path(), Network::Regtest);
        let descriptors = manager.registered_descriptors(fingerprint(99));
        assert!(
            descriptors.is_empty(),
            "expected an empty slice for an unknown fingerprint, got {descriptors:?}"
        );
    }
}
