//! In-memory view of every registered key on one network, backed by a `KeyStore`.
//!
//! `KeyManager` owns the records for one network and writes every change through to
//! disk before it updates memory. Everything that can be derived from a record and the
//! live device list, such as whether a key is connected, is computed at call time and
//! never cached.

use std::collections::{BTreeMap, BTreeSet};
use std::str::FromStr;

use bitcoin::{
    bip32::{DerivationPath, Fingerprint, Xpub},
    secp256k1::{All, Secp256k1},
    Network,
};
use bwk_sp::bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor;
use zeroize::Zeroizing;

use crate::keys::{
    crypto::{self, Password},
    error::Error,
    hot, network_name,
    psbt::{inspect_bytes, PsbtArtifacts},
    record::{DescriptorEntry, DeviceRecord, KeyRecord, RouteKind, SecretEnvelope, XpubEntry},
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
    /// Firmware version, empty when the module does not report one (hwi).
    pub version: String,
    /// bwk-qr capability bits, 0 when the module does not report them (hwi).
    pub capabilities: u32,
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
            version: self.version.clone(),
            capabilities: self.capabilities,
        }
    }
}

/// Why a candidate cannot sign right now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnusableReason {
    /// No key record exists for this fingerprint on this network.
    NotRegistered,
    /// The key exists but has no route at all.
    NoRoute,
    /// The only routes are USB and no matching device is connected.
    DeviceNotConnected,
    /// The only route is Hot and the record carries no secret.
    NoHotSecret,
    /// The SP key has not completed the output scripts yet.
    SpOutputsPending,
}

/// One fingerprint the PSBT requires, whether or not a key is registered for it and
/// whether or not it has a usable route right now.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SigningCandidate {
    pub fingerprint: Fingerprint,
    /// Empty when the key is not registered: there is no alias to show yet.
    pub alias: String,
    pub registered: bool,
    /// The route the signing flow would take, None when the key is unusable.
    pub route: Option<RouteKind>,
    /// The live device id, set only when `route` is `RouteKind::Usb`.
    pub device_id: String,
    /// This key completes the SP output scripts, so it acts before the others.
    pub must_sign_first: bool,
    /// None when the candidate can sign right now.
    pub unusable: Option<UnusableReason>,
}

/// What the signer picker is built from for one PSBT.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SigningPlan {
    /// One entry per fingerprint the PSBT requires, SP first when it must sign first,
    /// then in fingerprint order.
    pub candidates: Vec<SigningCandidate>,
    /// Connected devices whose fingerprint this PSBT does not need, so the UI can warn
    /// about a wrong device.
    pub unrelated_live: Vec<LiveDevice>,
    pub sp_fingerprint: Option<Fingerprint>,
    pub sp_outputs_pending: bool,
}

/// Names the exact cause instead of a catch-all: empty routes, hot with no secret, or
/// the remaining case of a route that needs a device that is not connected.
fn unusable_reason(record: &KeyRecord) -> UnusableReason {
    if record.routes.is_empty() {
        return UnusableReason::NoRoute;
    }
    if record.routes.as_slice() == [RouteKind::Hot] && record.secret.is_none() {
        return UnusableReason::NoHotSecret;
    }
    UnusableReason::DeviceNotConnected
}

pub trait KeyManagerImpl {
    fn network(&self) -> Network;

    fn keys(&self) -> Vec<KeyRecord>;
    fn key(&self, fingerprint: Fingerprint) -> Option<&KeyRecord>;
    fn add_key(&mut self, record: KeyRecord) -> Result<(), Error>;
    fn remove_key(&mut self, fingerprint: Fingerprint, force: bool) -> Result<(), Error>;
    fn set_alias(&mut self, fingerprint: Fingerprint, alias: String) -> Result<(), Error>;

    fn import_hot_key(
        &mut self,
        mnemonic: Zeroizing<String>,
        alias: String,
        password: Password,
    ) -> Result<Fingerprint, Error>;
    /// Stores a key `hot::seal_mnemonic` already sealed: a new record with the
    /// secret and the Hot route, or the existing record updated with both in the
    /// same store write.
    fn commit_hot_key(
        &mut self,
        fingerprint: Fingerprint,
        envelope: SecretEnvelope,
        alias: String,
    ) -> Result<(), Error>;
    fn reveal_mnemonic(
        &self,
        fingerprint: Fingerprint,
        password: Password,
    ) -> Result<Zeroizing<String>, Error>;

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

    fn required_fingerprints(&self, psbt_bytes: &[u8]) -> Result<BTreeSet<Fingerprint>, Error>;
    fn signing_plan(&self, psbt_bytes: &[u8]) -> Result<SigningPlan, Error>;
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

    fn candidate_for(
        &self,
        fingerprint: Fingerprint,
        artifacts: &PsbtArtifacts,
    ) -> SigningCandidate {
        let is_sp = artifacts.sp_fingerprint == Some(fingerprint);
        let must_sign_first = is_sp && artifacts.sp_outputs_pending;

        let record = match self.records.get(&fingerprint) {
            Some(record) => record,
            None => {
                return SigningCandidate {
                    fingerprint,
                    alias: String::new(),
                    registered: false,
                    route: None,
                    device_id: String::new(),
                    must_sign_first,
                    unusable: Some(UnusableReason::NotRegistered),
                };
            }
        };

        if artifacts.sp_outputs_pending && !is_sp {
            return SigningCandidate {
                fingerprint,
                alias: record.alias.clone(),
                registered: true,
                route: None,
                device_id: String::new(),
                must_sign_first: false,
                unusable: Some(UnusableReason::SpOutputsPending),
            };
        }

        match self.best_route(fingerprint) {
            Some(route) => {
                let device_id = if route == RouteKind::Usb {
                    self.live_device_for(fingerprint)
                        .map(|device| device.id.clone())
                        .unwrap_or_default()
                } else {
                    String::new()
                };
                SigningCandidate {
                    fingerprint,
                    alias: record.alias.clone(),
                    registered: true,
                    route: Some(route),
                    device_id,
                    must_sign_first,
                    unusable: None,
                }
            }
            None => SigningCandidate {
                fingerprint,
                alias: record.alias.clone(),
                registered: true,
                route: None,
                device_id: String::new(),
                must_sign_first,
                unusable: Some(unusable_reason(record)),
            },
        }
    }

    /// Every `sp(` descriptor registered on any key on this network, parsed. A
    /// descriptor that fails to parse is refused outright rather than skipped: a
    /// broken registration must not silently vanish from the signing plan.
    pub fn registered_sp_descriptors(&self) -> Result<Vec<SpDescriptor>, Error> {
        self.records
            .values()
            .flat_map(|record| record.descriptors.iter())
            .filter(|entry| entry.descriptor.starts_with("sp("))
            .map(|entry| {
                SpDescriptor::from_str(&entry.descriptor)
                    .map_err(|e| Error::DescriptorParse(e.to_string()))
            })
            .collect()
    }

    /// Splitting this from `signing_plan` keeps the plan testable without building a
    /// PSBT for every ordering case.
    pub fn plan_from_artifacts(&self, artifacts: &PsbtArtifacts) -> SigningPlan {
        let mut candidates: Vec<SigningCandidate> = artifacts
            .required
            .iter()
            .map(|fingerprint| self.candidate_for(*fingerprint, artifacts))
            .collect();

        if let Some(index) = candidates
            .iter()
            .position(|candidate| candidate.must_sign_first)
        {
            let sp_candidate = candidates.remove(index);
            candidates.insert(0, sp_candidate);
        }

        let unrelated_live = self
            .live
            .iter()
            .filter(|device| {
                device
                    .fingerprint
                    .is_some_and(|fingerprint| !artifacts.required.contains(&fingerprint))
            })
            .cloned()
            .collect();

        SigningPlan {
            candidates,
            unrelated_live,
            sp_fingerprint: artifacts.sp_fingerprint,
            sp_outputs_pending: artifacts.sp_outputs_pending,
        }
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

    fn import_hot_key(
        &mut self,
        mnemonic: Zeroizing<String>,
        alias: String,
        password: Password,
    ) -> Result<Fingerprint, Error> {
        let (fingerprint, envelope) = hot::seal_mnemonic(&mnemonic, self.network, password)?;
        self.commit_hot_key(fingerprint, envelope, alias)?;
        Ok(fingerprint)
    }

    fn commit_hot_key(
        &mut self,
        fingerprint: Fingerprint,
        envelope: SecretEnvelope,
        alias: String,
    ) -> Result<(), Error> {
        if !self.records.contains_key(&fingerprint) {
            let mut record = KeyRecord::new(fingerprint, self.network, alias);
            record.secret = Some(envelope.clone());
            record.routes.push(RouteKind::Hot)?;
            match self.add_key(record) {
                Ok(()) => return Ok(()),
                Err(Error::DuplicateKey(_)) => {}
                Err(e) => return Err(e),
            }
        }

        self.mutate(fingerprint, |record| {
            if record.secret.is_some() {
                return Err(Error::SecretAlreadyPresent(fingerprint));
            }
            record.secret = Some(envelope);
            record.routes.push(RouteKind::Hot)
        })
    }

    fn reveal_mnemonic(
        &self,
        fingerprint: Fingerprint,
        password: Password,
    ) -> Result<Zeroizing<String>, Error> {
        let record = self
            .records
            .get(&fingerprint)
            .ok_or(Error::UnknownKey(fingerprint))?;
        crypto::open_mnemonic(record, self.network, password)
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
            // A known device keeps its entry and takes the version and capabilities
            // it reports now.
            match record
                .devices
                .iter_mut()
                .find(|existing| existing.same_device(&device))
            {
                Some(existing) => *existing = device,
                None => record.devices.push(device),
            }
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

    fn required_fingerprints(&self, psbt_bytes: &[u8]) -> Result<BTreeSet<Fingerprint>, Error> {
        let descriptors = self.registered_sp_descriptors()?;
        let artifacts = inspect_bytes(psbt_bytes, &descriptors, &self.secp)?;
        Ok(artifacts.required)
    }

    fn signing_plan(&self, psbt_bytes: &[u8]) -> Result<SigningPlan, Error> {
        let descriptors = self.registered_sp_descriptors()?;
        let artifacts = inspect_bytes(psbt_bytes, &descriptors, &self.secp)?;
        Ok(self.plan_from_artifacts(&artifacts))
    }
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use super::*;
    use crate::keys::record::SecretEnvelope;

    fn fingerprint(value: u32) -> Fingerprint {
        Fingerprint::from(value.to_be_bytes())
    }

    fn manager_over(root: &std::path::Path, network: Network) -> KeyManager {
        let store = KeyStore::new(root.to_path_buf(), network);
        KeyManager::new(store, network).expect("build manager")
    }

    fn test_secret() -> SecretEnvelope {
        SecretEnvelope {
            version: 1,
            kdf: "argon2id".to_string(),
            m_cost_kib: 65_536,
            t_cost: 3,
            p_cost: 1,
            salt: "salt".to_string(),
            cipher: "xchacha20poly1305".to_string(),
            nonce: "nonce".to_string(),
            ciphertext: "ciphertext".to_string(),
        }
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
    fn add_key_catches_up_memory_from_a_stale_duplicate_on_disk() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager_a = manager_over(tmp.path(), Network::Regtest);
        let mut manager_b = manager_over(tmp.path(), Network::Regtest);

        let fp = fingerprint(1);
        let mut with_secret = KeyRecord::new(fp, Network::Regtest, "hot alias".to_string());
        with_secret.secret = Some(test_secret());
        with_secret
            .routes
            .push(RouteKind::Hot)
            .expect("push hot route");
        manager_a
            .add_key(with_secret.clone())
            .expect("A adds the hot key");

        let without_secret = KeyRecord::new(fp, Network::Regtest, "stale alias".to_string());
        let result = manager_b.add_key(without_secret);
        assert!(
            matches!(result, Err(Error::DuplicateKey(found)) if found == fp),
            "expected DuplicateKey, got {result:?}"
        );

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let on_disk = store.load(fp).expect("load record");
        assert_eq!(
            on_disk, with_secret,
            "the on-disk record must keep A's secret, got {on_disk:?}"
        );

        let refreshed = manager_b.key(fp).expect("B's memory refreshed from disk");
        assert_eq!(
            refreshed, &with_secret,
            "expected B's in-memory record to catch up to the on-disk one, got {refreshed:?}"
        );
    }

    #[test]
    fn add_key_refuses_a_record_written_directly_through_a_second_store_instance() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);

        let second_store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let mut written = KeyRecord::new(fp, Network::Regtest, "written directly".to_string());
        written.secret = Some(test_secret());
        written.routes.push(RouteKind::Hot).expect("push hot route");
        second_store
            .save(&written)
            .expect("second writer saves directly");

        let result = manager.add_key(KeyRecord::new(
            fp,
            Network::Regtest,
            "manager alias".to_string(),
        ));
        assert!(
            matches!(result, Err(Error::DuplicateKey(found)) if found == fp),
            "expected DuplicateKey, got {result:?}"
        );

        let on_disk = second_store.load(fp).expect("load record");
        assert_eq!(
            on_disk, written,
            "expected the directly written record to remain unchanged, got {on_disk:?}"
        );

        let refreshed = manager
            .key(fp)
            .expect("manager's memory refreshed from disk");
        assert_eq!(
            refreshed, &written,
            "expected the manager's memory to catch up to the on-disk record, got {refreshed:?}"
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
        let mut record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        record.secret = Some(test_secret());
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

    const TEST_MNEMONIC: &str = "abandon abandon abandon abandon abandon abandon \
        abandon abandon abandon abandon abandon about";
    const TEST_HOT_FINGERPRINT_BYTES: [u8; 4] = [0x73, 0xc5, 0xda, 0x0a];

    fn test_hot_fingerprint() -> Fingerprint {
        Fingerprint::from(TEST_HOT_FINGERPRINT_BYTES)
    }

    fn import_test_mnemonic(
        manager: &mut KeyManager,
        alias: &str,
        password: &str,
    ) -> Result<Fingerprint, Error> {
        manager.import_hot_key(
            Zeroizing::new(TEST_MNEMONIC.to_string()),
            alias.to_string(),
            Password::new(password.to_string()),
        )
    }

    #[test]
    fn import_hot_key_creates_the_record_with_hot_first() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);

        let fingerprint =
            import_test_mnemonic(&mut manager, "alias", "password").expect("import succeeds");
        assert_eq!(
            fingerprint,
            test_hot_fingerprint(),
            "expected the standard test vector's fingerprint, got {fingerprint}"
        );

        let key = manager.key(fingerprint).expect("key present");
        assert_eq!(
            key.routes.as_slice(),
            &[RouteKind::Hot],
            "expected only the hot route, got {:?}",
            key.routes.as_slice()
        );
        assert!(
            key.secret.is_some(),
            "expected the imported key to carry a secret"
        );
    }

    #[test]
    fn import_hot_key_attaches_to_an_existing_record() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = test_hot_fingerprint();
        manager
            .add_key(KeyRecord::new(
                fp,
                Network::Regtest,
                "usb alias".to_string(),
            ))
            .expect("add usb key");
        manager
            .add_route(fp, RouteKind::Usb)
            .expect("add usb route");

        import_test_mnemonic(&mut manager, "ignored alias", "password")
            .expect("import attaches to the existing record");

        let key = manager.key(fp).expect("key present");
        assert_eq!(
            key.routes.as_slice(),
            &[RouteKind::Hot, RouteKind::Usb],
            "expected hot first alongside the existing usb route, got {:?}",
            key.routes.as_slice()
        );
        assert_eq!(
            key.alias, "usb alias",
            "expected the original alias to be kept, got {}",
            key.alias
        );
        assert!(
            key.secret.is_some(),
            "expected the attached key to carry a secret"
        );
    }

    #[test]
    fn import_hot_key_refuses_to_overwrite_a_secret() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = import_test_mnemonic(&mut manager, "alias", "password").expect("first import");
        let before = manager
            .key(fp)
            .expect("key present")
            .secret
            .clone()
            .expect("secret present");

        let result = import_test_mnemonic(&mut manager, "alias", "other password");
        assert!(
            matches!(result, Err(Error::SecretAlreadyPresent(found)) if found == fp),
            "expected SecretAlreadyPresent, got {result:?}"
        );

        let after = manager
            .key(fp)
            .expect("key present")
            .secret
            .clone()
            .expect("secret present");
        assert_eq!(
            before, after,
            "expected the stored envelope to be unchanged after the refused import"
        );
    }

    #[test]
    fn import_hot_key_refuses_a_stale_secret_already_present_on_disk() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager_a = manager_over(tmp.path(), Network::Regtest);
        let fp = test_hot_fingerprint();
        manager_a
            .add_key(KeyRecord::new(
                fp,
                Network::Regtest,
                "usb alias".to_string(),
            ))
            .expect("add usb key");
        manager_a
            .add_route(fp, RouteKind::Usb)
            .expect("add usb route");

        let mut manager_b = manager_over(tmp.path(), Network::Regtest);

        import_test_mnemonic(&mut manager_a, "ignored alias", "password")
            .expect("A imports the secret");

        let result = import_test_mnemonic(&mut manager_b, "ignored alias", "other password");
        assert!(
            matches!(result, Err(Error::SecretAlreadyPresent(found)) if found == fp),
            "expected SecretAlreadyPresent, got {result:?}"
        );

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let on_disk = store.load(fp).expect("load record");
        let a_secret = manager_a
            .key(fp)
            .expect("key present")
            .secret
            .clone()
            .expect("secret present");
        assert_eq!(
            on_disk.secret,
            Some(a_secret),
            "expected the on-disk secret to be unchanged after B's refused import"
        );
    }

    #[test]
    fn import_hot_key_refuses_a_bad_mnemonic() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);

        let result = manager.import_hot_key(
            Zeroizing::new("not a mnemonic".to_string()),
            "alias".to_string(),
            Password::new("password".to_string()),
        );
        assert!(
            matches!(result, Err(Error::Mnemonic(_))),
            "expected a mnemonic error for an unparsable mnemonic, got {result:?}"
        );
        assert!(
            manager.keys().is_empty(),
            "expected no record to be created for a bad mnemonic, got {:?}",
            manager.keys()
        );
    }

    #[test]
    fn reveal_mnemonic_round_trips() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = import_test_mnemonic(&mut manager, "alias", "password").expect("import");

        let revealed = manager
            .reveal_mnemonic(fp, Password::new("password".to_string()))
            .expect("reveal succeeds");
        assert_eq!(
            revealed.as_str(),
            TEST_MNEMONIC,
            "expected the revealed mnemonic to match the imported one, got {}",
            revealed.as_str()
        );
    }

    #[test]
    fn reveal_with_a_wrong_password_errors() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = import_test_mnemonic(&mut manager, "alias", "password").expect("import");

        let result = manager.reveal_mnemonic(fp, Password::new("wrong".to_string()));
        assert!(
            matches!(result, Err(Error::WrongPassword)),
            "expected WrongPassword, got {result:?}"
        );
    }

    #[test]
    fn reveal_without_a_secret_errors() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");

        let result = manager.reveal_mnemonic(fp, Password::new("password".to_string()));
        assert!(
            matches!(result, Err(Error::NoSecret(found)) if found == fp),
            "expected NoSecret, got {result:?}"
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
            version: String::new(),
            capabilities: 0,
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
    fn record_device_refreshes_a_known_device() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let record = KeyRecord::new(fingerprint(1), Network::Regtest, "alias".to_string());
        manager.add_key(record).expect("add key");

        let device = DeviceRecord {
            kind: "vendor".to_string(),
            model: "model".to_string(),
            serial: String::new(),
            module: "qr".to_string(),
            version: "1.0.0".to_string(),
            capabilities: 0x0000_0001,
        };
        manager
            .record_device(fingerprint(1), device.clone())
            .expect("first record");
        let mut upgraded = device.clone();
        upgraded.version = "1.1.0".to_string();
        upgraded.capabilities = 0x0000_0003;
        manager
            .record_device(fingerprint(1), upgraded.clone())
            .expect("upgraded record");

        let store = KeyStore::new(tmp.path().to_path_buf(), Network::Regtest);
        let on_disk = store.load(fingerprint(1)).expect("load record");
        assert_eq!(
            on_disk.devices,
            vec![upgraded],
            "expected the known device refreshed in place, got {:?}",
            on_disk.devices
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
            version: String::new(),
            capabilities: 0,
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
            version: String::new(),
            capabilities: 0,
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
        // The store refuses to persist a hot route without a secret, so this
        // unusable state is simulated in memory only to exercise best_route's
        // handling of it.
        manager
            .records
            .get_mut(&fingerprint(2))
            .expect("record present")
            .routes
            .push(RouteKind::Hot)
            .expect("push hot route");
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
            version: String::new(),
            capabilities: 0,
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

    fn live_device(id: &str, serial: &str, fp: Option<Fingerprint>) -> LiveDevice {
        LiveDevice {
            id: id.to_string(),
            kind: "hardware".to_string(),
            model: "model".to_string(),
            serial: serial.to_string(),
            module: "hwi".to_string(),
            version: String::new(),
            capabilities: 0,
            fingerprint: fp,
        }
    }

    /// One SP fingerprint and two other registered, USB-connected fingerprints, with
    /// `sp_outputs_pending` true. Shared by the tests that check the SP-first rule.
    fn sp_pending_scenario() -> (KeyManager, PsbtArtifacts) {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let sp_fp = fingerprint(1);
        let other_a = fingerprint(2);
        let other_b = fingerprint(3);

        for fp in [sp_fp, other_a, other_b] {
            manager
                .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
                .expect("add key");
        }
        for fp in [other_a, other_b] {
            manager
                .add_route(fp, RouteKind::Usb)
                .expect("add usb route");
        }
        manager.set_live_devices(vec![
            live_device("dev-a", "serial-a", Some(other_a)),
            live_device("dev-b", "serial-b", Some(other_b)),
        ]);

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([sp_fp, other_a, other_b]),
            sp_fingerprint: Some(sp_fp),
            sp_inputs: true,
            sp_outputs: true,
            sp_outputs_pending: true,
        };

        (manager, artifacts)
    }

    #[test]
    fn every_required_fingerprint_gets_a_candidate() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        manager
            .add_key(KeyRecord::new(
                fingerprint(1),
                Network::Regtest,
                "alias".to_string(),
            ))
            .expect("add key");

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([fingerprint(1), fingerprint(2)]),
            sp_fingerprint: None,
            sp_inputs: false,
            sp_outputs: false,
            sp_outputs_pending: false,
        };
        let plan = manager.plan_from_artifacts(&artifacts);
        assert_eq!(
            plan.candidates.len(),
            2,
            "expected one candidate per required fingerprint, got {:?}",
            plan.candidates
        );

        let unregistered = plan
            .candidates
            .iter()
            .find(|candidate| candidate.fingerprint == fingerprint(2))
            .expect("unregistered candidate present");
        assert_eq!(
            unregistered.unusable,
            Some(UnusableReason::NotRegistered),
            "expected NotRegistered for the unregistered fingerprint, got {:?}",
            unregistered.unusable
        );
        assert!(
            !unregistered.registered,
            "expected registered to be false for the unregistered fingerprint, got {unregistered:?}"
        );
    }

    #[test]
    fn sp_outputs_pending_blocks_every_other_key() {
        let (manager, artifacts) = sp_pending_scenario();
        let plan = manager.plan_from_artifacts(&artifacts);
        let sp_fp = artifacts.sp_fingerprint.expect("sp fingerprint set");

        for candidate in plan
            .candidates
            .iter()
            .filter(|candidate| candidate.fingerprint != sp_fp)
        {
            assert_eq!(
                candidate.unusable,
                Some(UnusableReason::SpOutputsPending),
                "expected SpOutputsPending for {candidate:?}"
            );
            assert_eq!(
                candidate.route, None,
                "expected no route while sp outputs are pending, got {candidate:?}"
            );
        }
    }

    #[test]
    fn the_sp_key_must_sign_first_and_is_listed_first() {
        let (manager, artifacts) = sp_pending_scenario();
        let plan = manager.plan_from_artifacts(&artifacts);
        let sp_fp = artifacts.sp_fingerprint.expect("sp fingerprint set");

        assert_eq!(
            plan.candidates[0].fingerprint, sp_fp,
            "expected the sp candidate first, got {:?}",
            plan.candidates
        );
        assert!(
            plan.candidates[0].must_sign_first,
            "expected the sp candidate to have must_sign_first set, got {:?}",
            plan.candidates[0]
        );
        let other_must_sign_first = plan.candidates[1..]
            .iter()
            .filter(|candidate| candidate.must_sign_first)
            .count();
        assert_eq!(
            other_must_sign_first, 0,
            "expected no other candidate with must_sign_first set, got {:?}",
            plan.candidates
        );
    }

    #[test]
    fn once_the_outputs_are_derived_everyone_can_sign() {
        let (manager, mut artifacts) = sp_pending_scenario();
        artifacts.sp_outputs_pending = false;
        let plan = manager.plan_from_artifacts(&artifacts);

        for candidate in &plan.candidates {
            assert_ne!(
                candidate.unusable,
                Some(UnusableReason::SpOutputsPending),
                "expected no SpOutputsPending once outputs are derived, got {candidate:?}"
            );
            assert!(
                !candidate.must_sign_first,
                "expected no must_sign_first once outputs are derived, got {candidate:?}"
            );
        }

        let order: Vec<Fingerprint> = plan.candidates.iter().map(|c| c.fingerprint).collect();
        let mut sorted = order.clone();
        sorted.sort();
        assert_eq!(
            order, sorted,
            "expected plain fingerprint order, got {order:?}"
        );
    }

    #[test]
    fn a_usb_candidate_names_the_device() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");
        manager
            .add_route(fp, RouteKind::Usb)
            .expect("add usb route");
        manager.set_live_devices(vec![live_device("dev-1", "serial", Some(fp))]);

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([fp]),
            sp_fingerprint: None,
            sp_inputs: false,
            sp_outputs: false,
            sp_outputs_pending: false,
        };
        let plan = manager.plan_from_artifacts(&artifacts);
        let candidate = &plan.candidates[0];
        assert_eq!(
            candidate.route,
            Some(RouteKind::Usb),
            "expected a usb route, got {:?}",
            candidate.route
        );
        assert!(
            !candidate.device_id.is_empty(),
            "expected a non-empty device id for a usb candidate, got {candidate:?}"
        );
    }

    #[test]
    fn a_non_usb_candidate_leaves_the_device_id_empty() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");
        manager.add_route(fp, RouteKind::Qr).expect("add qr route");

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([fp]),
            sp_fingerprint: None,
            sp_inputs: false,
            sp_outputs: false,
            sp_outputs_pending: false,
        };
        let plan = manager.plan_from_artifacts(&artifacts);
        let candidate = &plan.candidates[0];
        assert_eq!(
            candidate.route,
            Some(RouteKind::Qr),
            "expected a qr route, got {:?}",
            candidate.route
        );
        assert!(
            candidate.device_id.is_empty(),
            "expected an empty device id for a non-usb candidate, got {}",
            candidate.device_id
        );
    }

    #[test]
    fn each_unusable_reason_is_named_exactly() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let no_route = fingerprint(1);
        let no_hot_secret = fingerprint(2);
        let device_not_connected = fingerprint(3);

        manager
            .add_key(KeyRecord::new(
                no_route,
                Network::Regtest,
                "alias".to_string(),
            ))
            .expect("add no route key");
        manager
            .add_key(KeyRecord::new(
                no_hot_secret,
                Network::Regtest,
                "alias".to_string(),
            ))
            .expect("add hot key");
        // The store now refuses to persist a hot route without a secret, so this
        // fallback state (data written before that guard existed, or corrupted on
        // disk) is simulated in memory only, to exercise unusable_reason's handling
        // of it rather than the store's write path.
        manager
            .records
            .get_mut(&no_hot_secret)
            .expect("record present")
            .routes
            .push(RouteKind::Hot)
            .expect("push hot route");
        manager
            .add_key(KeyRecord::new(
                device_not_connected,
                Network::Regtest,
                "alias".to_string(),
            ))
            .expect("add usb key");
        manager
            .add_route(device_not_connected, RouteKind::Usb)
            .expect("add usb route");

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([no_route, no_hot_secret, device_not_connected]),
            sp_fingerprint: None,
            sp_inputs: false,
            sp_outputs: false,
            sp_outputs_pending: false,
        };
        let plan = manager.plan_from_artifacts(&artifacts);

        let reason_for = |fp: Fingerprint| {
            plan.candidates
                .iter()
                .find(|candidate| candidate.fingerprint == fp)
                .expect("candidate present")
                .unusable
        };
        assert_eq!(
            reason_for(no_route),
            Some(UnusableReason::NoRoute),
            "expected NoRoute for a key with no routes"
        );
        assert_eq!(
            reason_for(no_hot_secret),
            Some(UnusableReason::NoHotSecret),
            "expected NoHotSecret for a hot-only key with no secret"
        );
        assert_eq!(
            reason_for(device_not_connected),
            Some(UnusableReason::DeviceNotConnected),
            "expected DeviceNotConnected for a usb key with no live device"
        );
    }

    #[test]
    fn route_and_unusable_are_mutually_exclusive() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let usable = fingerprint(1);
        let unregistered = fingerprint(2);
        let no_route = fingerprint(3);

        manager
            .add_key(KeyRecord::new(
                usable,
                Network::Regtest,
                "alias".to_string(),
            ))
            .expect("add usable key");
        manager
            .add_route(usable, RouteKind::Qr)
            .expect("add qr route");
        manager
            .add_key(KeyRecord::new(
                no_route,
                Network::Regtest,
                "alias".to_string(),
            ))
            .expect("add no route key");

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([usable, unregistered, no_route]),
            sp_fingerprint: None,
            sp_inputs: false,
            sp_outputs: false,
            sp_outputs_pending: false,
        };
        let plan = manager.plan_from_artifacts(&artifacts);

        let (sp_manager, sp_artifacts) = sp_pending_scenario();
        let sp_plan = sp_manager.plan_from_artifacts(&sp_artifacts);

        for candidate in plan.candidates.iter().chain(sp_plan.candidates.iter()) {
            assert!(
                candidate.route.is_some() != candidate.unusable.is_some(),
                "expected exactly one of route/unusable set, got {candidate:?}"
            );
        }
    }

    #[test]
    fn unrelated_live_devices_are_listed() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let required_fp = fingerprint(1);
        let unrelated_fp = fingerprint(2);
        manager.set_live_devices(vec![
            live_device("dev-required", "serial-required", Some(required_fp)),
            live_device("dev-unrelated", "serial-unrelated", Some(unrelated_fp)),
        ]);

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([required_fp]),
            sp_fingerprint: None,
            sp_inputs: false,
            sp_outputs: false,
            sp_outputs_pending: false,
        };
        let plan = manager.plan_from_artifacts(&artifacts);
        assert_eq!(
            plan.unrelated_live.len(),
            1,
            "expected only the unrelated device listed, got {:?}",
            plan.unrelated_live
        );
        assert_eq!(
            plan.unrelated_live[0].fingerprint,
            Some(unrelated_fp),
            "expected the unrelated device's fingerprint, got {:?}",
            plan.unrelated_live[0]
        );
    }

    #[test]
    fn a_live_device_with_no_fingerprint_is_not_unrelated() {
        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        manager.set_live_devices(vec![live_device("dev-locked", "serial", None)]);

        let artifacts = PsbtArtifacts {
            required: BTreeSet::from([fingerprint(1)]),
            sp_fingerprint: None,
            sp_inputs: false,
            sp_outputs: false,
            sp_outputs_pending: false,
        };
        let plan = manager.plan_from_artifacts(&artifacts);
        assert!(
            plan.unrelated_live.is_empty(),
            "expected a fingerprint-less device to be excluded from unrelated_live, got {:?}",
            plan.unrelated_live
        );
    }

    #[test]
    fn signing_plan_from_bytes_matches_plan_from_artifacts() {
        use bitcoin::hashes::Hash;
        use bwk_sp::bwk_tx::bwk_psbt::{Input as PsbtInput, PsbtV2};

        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");

        let secp = bitcoin::secp256k1::Secp256k1::new();
        let public_key = bitcoin::secp256k1::PublicKey::from_secret_key(
            &secp,
            &bitcoin::secp256k1::SecretKey::from_slice(&[0x21; 32]).expect("valid key"),
        );
        let path = (
            fp,
            DerivationPath::from(vec![
                bitcoin::bip32::ChildNumber::from_hardened_idx(0).expect("valid index")
            ]),
        );
        let mut input = PsbtInput {
            previous_output: bitcoin::OutPoint::new(bitcoin::Txid::from_byte_array([0x11; 32]), 0),
            sequence: bitcoin::Sequence::MAX,
            required_time_lock_time: None,
            required_height_lock_time: None,
            psbt: bitcoin::psbt::Input::default(),
        };
        input.psbt.bip32_derivation.insert(public_key, path);

        let psbt = PsbtV2 {
            tx_version: bitcoin::transaction::Version::TWO,
            fallback_lock_time: Some(bitcoin::absolute::LockTime::ZERO),
            tx_modifiable: None,
            xpub: Default::default(),
            proprietary: Default::default(),
            unknown: Default::default(),
            inputs: vec![input],
            outputs: Vec::new(),
        };
        let bytes = psbt.serialize().expect("serialize psbt");

        let artifacts = inspect_bytes(&bytes, &[], &manager.secp).expect("inspect bytes");
        let expected = manager.plan_from_artifacts(&artifacts);
        let found = manager
            .signing_plan(&bytes)
            .expect("signing plan from bytes");
        assert_eq!(
            found, expected,
            "signing_plan from bytes must match plan_from_artifacts, got {found:?} expected {expected:?}"
        );
    }

    #[test]
    fn set_descriptor_registered_refuses_a_spend_descriptor_but_accepts_watch_only() {
        use bitcoin::bip32::DerivationPath;
        use bwk_sp::bwk_sign::bwk_descriptor::sp_key::{SpKey, SpSpendKey};
        use bwk_sp::bwk_sign::hot_signer::HotSigner;
        use bwk_sp::receiver::{bip352_base_derivation, scan_path, spend_path};

        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = import_test_mnemonic(&mut manager, "alias", "password").expect("import");

        let signer =
            HotSigner::new_from_mnemonics(Network::Regtest, TEST_MNEMONIC).expect("signer builds");
        let account = hot::sp_account().expect("sp account index");
        let origin = (
            signer.fingerprint(),
            DerivationPath::from(bip352_base_derivation(Network::Regtest, account)),
        );
        let spend_descriptor = SpDescriptor::Packed {
            origin: Some(origin),
            key: SpKey::Spend(SpSpendKey {
                scan_key: signer.private_key_at(&scan_path(Network::Regtest, account)),
                spend_key: signer.private_key_at(&spend_path(Network::Regtest, account)),
                network: bitcoin::NetworkKind::Test,
            }),
        }
        .to_string();

        let result = manager.set_descriptor_registered(fp, "alias".to_string(), spend_descriptor);
        assert!(
            matches!(result, Err(Error::SpSpendDescriptor)),
            "expected SpSpendDescriptor, got {result:?}"
        );
        assert!(
            manager.registered_descriptors(fp).is_empty(),
            "expected nothing written for a refused spend descriptor, got {:?}",
            manager.registered_descriptors(fp)
        );

        let watch_only_descriptor = hot::sp_descriptor(
            manager.key(fp).expect("key present"),
            Network::Regtest,
            Password::new("password".to_string()),
        )
        .expect("watch-only descriptor builds")
        .to_string();

        manager
            .set_descriptor_registered(fp, "alias".to_string(), watch_only_descriptor.clone())
            .expect("watch-only descriptor is accepted");
        assert_eq!(
            manager.registered_descriptors(fp),
            &[DescriptorEntry {
                alias: "alias".to_string(),
                descriptor: watch_only_descriptor,
            }],
            "expected the watch-only descriptor to register"
        );
    }

    #[test]
    fn required_fingerprints_errors_on_a_malformed_registered_sp_descriptor() {
        use bwk_sp::bwk_tx::bwk_psbt::PsbtV2;

        let tmp = temp_dir::TempDir::new().expect("create temp dir");
        let mut manager = manager_over(tmp.path(), Network::Regtest);
        let fp = fingerprint(1);
        manager
            .add_key(KeyRecord::new(fp, Network::Regtest, "alias".to_string()))
            .expect("add key");
        manager
            .set_descriptor_registered(
                fp,
                "alias".to_string(),
                "sp(not a valid descriptor".to_string(),
            )
            .expect("registration itself does not validate parseability");

        let psbt = PsbtV2 {
            tx_version: bitcoin::transaction::Version::TWO,
            fallback_lock_time: Some(bitcoin::absolute::LockTime::ZERO),
            tx_modifiable: None,
            xpub: Default::default(),
            proprietary: Default::default(),
            unknown: Default::default(),
            inputs: Vec::new(),
            outputs: Vec::new(),
        };
        let bytes = psbt.serialize().expect("serialize psbt");

        let result = manager.required_fingerprints(&bytes);
        assert!(
            matches!(result, Err(Error::DescriptorParse(_))),
            "expected DescriptorParse rather than silently skipping the malformed \
             descriptor, got {result:?}"
        );
    }
}
