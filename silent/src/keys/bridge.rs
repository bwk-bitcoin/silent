//! CXX bridge for the keys module: module lifecycle, key records, routes, live
//! devices, the xpub cache and registered descriptors.
//!
//! The only file under `keys/` that names `cxx` or `crate::ffi`. Everything here
//! forwards to `KeyManager`; no policy lives in this file beyond parsing FFI inputs
//! and validating what the manager has no way to check on its own (an xpub or
//! descriptor's own cryptographic identity).

use std::{
    collections::{BTreeMap, BTreeSet},
    path::Path,
    str::FromStr,
};

use base64::{prelude::BASE64_STANDARD, Engine};
use bitcoin::{
    bip32::{DerivationPath, Fingerprint, Xpub},
    Network as BtcNetwork,
};
use bwk::{
    bwk_descriptor::{musig::parse_musig_descriptor, wallet::WalletDescriptorExt},
    bwk_keys::musig::MusigKey,
    bwk_sign::{miniscript, musig::own_slots},
};
use bwk_qr_protocol::{request, response};
use bwk_sp::bwk_sign::bwk_descriptor::descriptor::Descriptor;
use bwk_sp::bwk_tx::bwk_psbt::Error as PsbtV2Error;
use zeroize::Zeroizing;

use crate::{
    config::{datadir, list_configs, Config},
    error::BackendFailure,
    ffi,
    hwi::{parse_hmac, wallet_name},
    keys::{
        crypto::{open_mnemonic, Password},
        hot::{
            create_sessions, publish_cached, seal_mnemonic, sign_hot, sp_descriptor, xpub_at,
            CachedPublication,
        },
        ledger_nonces::{self, Fetch, SessionSource, Signing},
        manager::{
            descriptor_has_key, KeyManager, KeyManagerImpl, LiveDevice, SigningCandidate, UsedXpub,
        },
        musig_sessions,
        portable::{self, PortableError},
        psbt, qr,
        record::{validate_xpub_chain, KeyRecord, RouteKind, SecretEnvelope, XpubEntry},
        store::KeyStore,
        Error, KEYS_DIR_NAME,
    },
};

/// Owns the manager and the QR session. The only file in `keys/` that names `cxx` or
/// `crate::ffi`.
pub struct KeysModule {
    inner: Option<KeyManager>,
    error: String,
    session: qr::QrSession,
}

pub struct AccountXpubUsage {
    network: BtcNetwork,
    used: Vec<UsedXpub>,
    error: String,
}

impl AccountXpubUsage {
    pub fn is_ok(&self) -> bool {
        self.error.is_empty()
    }
    pub fn get_error(&self) -> String {
        self.error.clone()
    }
}

pub struct KeyFilePreview {
    record: Result<KeyRecord, PortableError>,
}

pub struct KeyFileCommit {
    record: Result<KeyRecord, String>,
    kind: ffi::BackendErrorKind,
}

pub fn key_file_preview(path: &str, network: ffi::Network) -> Box<KeyFilePreview> {
    Box::new(KeyFilePreview {
        record: portable::read(Path::new(path), network.into()),
    })
}

impl KeyFilePreview {
    pub fn is_ok(&self) -> bool {
        self.record.is_ok()
    }
    pub fn get_error(&self) -> String {
        self.record
            .as_ref()
            .err()
            .map(ToString::to_string)
            .unwrap_or_default()
    }
    pub fn file_failure(&self) -> ffi::KeyFileFailure {
        match &self.record {
            Ok(_) => ffi::KeyFileFailure::None,
            Err(PortableError::Read(_)) => ffi::KeyFileFailure::Read,
            Err(PortableError::Version(_)) => ffi::KeyFileFailure::Newer,
            Err(PortableError::Network { .. }) => ffi::KeyFileFailure::Network,
            Err(_) => ffi::KeyFileFailure::Invalid,
        }
    }
    pub fn file_network(&self) -> ffi::Network {
        match &self.record {
            Ok(record) => record.network.into(),
            Err(PortableError::Network { found, .. }) => (*found).into(),
            Err(_) => ffi::Network::Regtest,
        }
    }
    pub fn file_fingerprint(&self) -> String {
        self.record
            .as_ref()
            .map(|r| r.fingerprint.to_string())
            .unwrap_or_default()
    }
    pub fn file_alias(&self) -> String {
        self.record
            .as_ref()
            .map(|r| r.alias.clone())
            .unwrap_or_default()
    }
    pub fn file_xpubs(&self) -> Vec<ffi::KeyXpub> {
        match &self.record {
            Ok(record) => record
                .xpubs
                .iter()
                .map(|entry| ffi::KeyXpub {
                    fingerprint: record.fingerprint.to_string(),
                    path: entry.path.to_string(),
                    xpub: entry.xpub.to_string(),
                    used: entry.used,
                })
                .collect(),
            Err(_) => Vec::new(),
        }
    }
}

pub fn commit_key_file(preview: &KeyFilePreview, alias: &str) -> Box<KeyFileCommit> {
    let record = (|| -> Result<KeyRecord, BackendFailure> {
        let mut record = preview
            .record
            .as_ref()
            .map_err(|error| {
                BackendFailure::new(ffi::BackendErrorKind::InvalidInput, error.to_string())
            })?
            .clone();
        record.alias = alias.trim().to_string();
        portable::validate(&record)
            .map_err(|e| BackendFailure::new(ffi::BackendErrorKind::InvalidInput, e.to_string()))?;
        open_manager(record.network.into())
            .map_err(|e| BackendFailure::new(ffi::BackendErrorKind::Storage, e))?
            .import_portable(record)
            .map_err(|e| {
                let kind = match &e {
                    Error::Io { .. } | Error::Parse { .. } | Error::State(_) => {
                        ffi::BackendErrorKind::Storage
                    }
                    _ => ffi::BackendErrorKind::InvalidInput,
                };
                BackendFailure::new(kind, e.to_string())
            })
    })();
    Box::new(match record {
        Ok(record) => KeyFileCommit {
            record: Ok(record),
            kind: ffi::BackendErrorKind::None,
        },
        Err(error) => KeyFileCommit {
            record: Err(error.message),
            kind: error.kind,
        },
    })
}

impl KeyFileCommit {
    pub fn get_error_kind(&self) -> ffi::BackendErrorKind {
        self.kind
    }
    pub fn is_ok(&self) -> bool {
        self.record.is_ok()
    }
    pub fn get_error(&self) -> String {
        self.record.as_ref().err().cloned().unwrap_or_default()
    }
}

pub fn new_keys_module(network: ffi::Network) -> Box<KeysModule> {
    Box::new(build_module(network))
}

/// Opens the manager for `network`.
fn open_manager(network: ffi::Network) -> Result<KeyManager, String> {
    let network = BtcNetwork::from(network);
    let store = KeyStore::new(datadir().join(KEYS_DIR_NAME), network);
    KeyManager::new(store, network).map_err(|e| format!("failed to open key store: {e}"))
}

fn build_module(network: ffi::Network) -> KeysModule {
    let (inner, error) = match open_manager(network) {
        Ok(manager) => (Some(manager), String::new()),
        Err(error) => (None, error),
    };
    KeysModule {
        inner,
        error,
        session: qr::QrSession::new(),
    }
}

impl KeysModule {
    pub fn apply_key_file(&mut self, committed: &KeyFileCommit) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        match &committed.record {
            Ok(record) => match manager.accept_imported(record.clone()) {
                Ok(()) => ok_op(),
                Err(error) => failed_op(&error.to_string()),
            },
            Err(error) => failed_op(error),
        }
    }
    pub fn is_ok(&self) -> bool {
        self.inner.is_some()
    }

    pub fn get_error(&self) -> String {
        self.error.clone()
    }

    /// Reloads the manager for `network`. On failure the previously-loaded
    /// manager (and every key it held) is left untouched; only `self.error` is
    /// updated, so a failed switch never bricks keys that were already loaded.
    /// The QR session starts over: a request issued on the old network is void.
    pub fn set_network(&mut self, network: ffi::Network) -> ffi::KeyOpResult {
        match open_manager(network) {
            Ok(manager) => {
                self.session.reset();
                self.inner = Some(manager);
                self.error = String::new();
                ok_op()
            }
            Err(error) => {
                self.error = error.clone();
                failed_op(&error)
            }
        }
    }

    /// Returns the manager's current network. If the module has no inner
    /// manager (see `is_ok()`), this returns `Regtest` as a sentinel: callers
    /// MUST check `is_ok()` / `get_error()` before trusting this value.
    pub fn network(&self) -> ffi::Network {
        match self.inner.as_ref() {
            Some(manager) => manager.network().into(),
            None => {
                log::error!("KeysModule::network(): {}", self.error);
                ffi::Network::Regtest
            }
        }
    }

    pub fn keys(&self) -> Vec<ffi::KeyInfo> {
        let Some(manager) = self.inner.as_ref() else {
            log::error!("KeysModule::keys(): {}", self.error);
            return Vec::new();
        };
        manager
            .keys()
            .iter()
            .map(|record| to_key_info(record, manager))
            .collect()
    }

    pub fn key(&self, fingerprint: &str) -> ffi::KeyInfoResult {
        let Some(manager) = self.inner.as_ref() else {
            return failed_key_info(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_key_info(&e),
        };
        match manager.key(fp) {
            Some(record) => ok_key_info(to_key_info(record, manager)),
            None => failed_key_info(&format!("key '{fingerprint}' is not registered")),
        }
    }

    pub fn add_key(&mut self, fingerprint: &str, alias: &str) -> ffi::KeyInfoResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_key_info(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_key_info(&e),
        };
        let record = KeyRecord::new(fp, manager.network(), alias.to_string());
        if let Err(e) = manager.add_key(record) {
            return failed_key_info(&format!("add_key {fingerprint}: {e}"));
        }
        match manager.key(fp) {
            Some(record) => ok_key_info(to_key_info(record, manager)),
            None => failed_key_info("key vanished immediately after being added"),
        }
    }

    pub fn remove_key(&mut self, fingerprint: &str, force: bool) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        match manager.remove_key(fp, force) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("remove_key {fingerprint}: {e}")),
        }
    }

    pub fn set_alias(&mut self, fingerprint: &str, alias: &str) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        match manager.set_alias(fp, alias.to_string()) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("set_alias {fingerprint}: {e}")),
        }
    }

    pub fn add_route(&mut self, fingerprint: &str, kind: ffi::KeyRouteKind) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let kind = match RouteKind::try_from(kind) {
            Ok(kind) => kind,
            Err(e) => return failed_op(&e),
        };
        match manager.add_route(fp, kind) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("add_route {fingerprint}: {e}")),
        }
    }

    pub fn remove_route(&mut self, fingerprint: &str, kind: ffi::KeyRouteKind) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let kind = match RouteKind::try_from(kind) {
            Ok(kind) => kind,
            Err(e) => return failed_op(&e),
        };
        match manager.remove_route(fp, kind) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("remove_route {fingerprint}: {e}")),
        }
    }

    pub fn set_route_order(
        &mut self,
        fingerprint: &str,
        order: Vec<ffi::KeyRouteKind>,
    ) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let order: Vec<RouteKind> = match order.into_iter().map(RouteKind::try_from).collect() {
            Ok(order) => order,
            Err(e) => return failed_op(&e),
        };
        match manager.set_route_order(fp, order) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("set_route_order {fingerprint}: {e}")),
        }
    }

    pub fn set_live_devices(&mut self, devices: Vec<ffi::LiveDevice>) {
        let Some(manager) = self.inner.as_mut() else {
            log::error!("KeysModule::set_live_devices(): {}", self.error);
            return;
        };
        let live = devices.into_iter().map(to_manager_live_device).collect();
        manager.set_live_devices(live);
    }

    pub fn live_devices(&self) -> Vec<ffi::LiveDevice> {
        let Some(manager) = self.inner.as_ref() else {
            log::error!("KeysModule::live_devices(): {}", self.error);
            return Vec::new();
        };
        manager
            .live_devices()
            .iter()
            .map(to_ffi_live_device)
            .collect()
    }

    pub fn record_device(&mut self, device: ffi::LiveDevice) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(&device.fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let record = to_manager_live_device(device).to_record();
        match manager.record_device(fp, record) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("record_device: {e}")),
        }
    }

    pub fn key_devices(&self, fingerprint: &str) -> Vec<ffi::KeyDevice> {
        let Some(manager) = self.inner.as_ref() else {
            log::error!("KeysModule::key_devices(): {}", self.error);
            return Vec::new();
        };
        let Ok(fp) = parse_fingerprint(fingerprint) else {
            log::error!("KeysModule::key_devices(): invalid fingerprint '{fingerprint}'");
            return Vec::new();
        };
        let Some(record) = manager.key(fp) else {
            return Vec::new();
        };
        record
            .devices
            .iter()
            .map(|device| ffi::KeyDevice {
                fingerprint: fingerprint.to_string(),
                // DeviceRecord carries neither a stable id nor a brand: only what
                // bwk-hwi reported at record time is stored.
                device_id: String::new(),
                kind: device.kind.clone(),
                brand: String::new(),
                model: device.model.clone(),
                serial: device.serial.clone(),
                module: device.module.clone(),
                version: device.version.clone(),
                capabilities: device.capabilities,
            })
            .collect()
    }

    pub fn cache_xpub(&mut self, fingerprint: &str, path: &str, xpub: &str) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let deriv_path = match DerivationPath::from_str(path) {
            Ok(deriv_path) => deriv_path,
            Err(e) => return failed_op(&format!("invalid derivation path '{path}': {e}")),
        };
        let xpub_value = match Xpub::from_str(xpub) {
            Ok(xpub_value) => xpub_value,
            Err(e) => return failed_op(&format!("invalid xpub '{xpub}': {e}")),
        };

        let Some(record) = manager.key(fp) else {
            return failed_op(&format!("key '{fingerprint}' is not registered"));
        };
        let mut entries = record.xpubs.clone();
        entries.push(XpubEntry {
            path: deriv_path.clone(),
            xpub: xpub_value,
            used: false,
        });
        if let Err(error) = validate_xpub_chain(&entries) {
            return failed_op(&error.to_string());
        }

        match manager.cache_xpub(fp, deriv_path, xpub_value) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("cache_xpub {fingerprint}: {e}")),
        }
    }

    pub fn remove_xpub(&mut self, fingerprint: &str, path: &str) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let deriv_path = match DerivationPath::from_str(path) {
            Ok(deriv_path) => deriv_path,
            Err(e) => return failed_op(&format!("invalid derivation path '{path}': {e}")),
        };
        match manager.remove_xpub(fp, &deriv_path) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("remove_xpub {fingerprint}: {e}")),
        }
    }

    pub fn key_xpubs(&self, fingerprint: &str) -> Vec<ffi::KeyXpub> {
        let Some(manager) = self.inner.as_ref() else {
            log::error!("KeysModule::key_xpubs(): {}", self.error);
            return Vec::new();
        };
        let Ok(fp) = parse_fingerprint(fingerprint) else {
            log::error!("KeysModule::key_xpubs(): invalid fingerprint '{fingerprint}'");
            return Vec::new();
        };
        let Some(record) = manager.key(fp) else {
            return Vec::new();
        };
        record
            .xpubs
            .iter()
            .map(|entry| ffi::KeyXpub {
                fingerprint: fingerprint.to_string(),
                path: entry.path.to_string(),
                xpub: entry.xpub.to_string(),
                used: entry.used,
            })
            .collect()
    }

    /// Applies usage metadata already persisted by the account discovery worker.
    pub fn apply_xpub_usage(&mut self, usage: &AccountXpubUsage) -> bool {
        self.inner
            .as_mut()
            .filter(|manager| manager.network() == usage.network)
            .is_some_and(|manager| manager.merge_used_xpubs(&usage.used))
    }

    pub fn register_descriptor(&mut self, fingerprint: &str, descriptor: &str) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match registration_key(fingerprint, descriptor) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let alias = wallet_name(descriptor);
        match manager.set_descriptor_registered(fp, alias, descriptor.to_string()) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("register_descriptor {fingerprint}: {e}")),
        }
    }

    pub fn set_descriptor_hmac(
        &mut self,
        fingerprint: &str,
        descriptor: &str,
        hmac: &str,
    ) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match registration_key(fingerprint, descriptor) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let hmac = match parse_hmac(hmac) {
            Ok(Some(hmac)) => hmac,
            Ok(None) => return failed_op("invalid hmac: empty"),
            Err(e) => return failed_op(&format!("invalid hmac: {e}")),
        };
        let alias = wallet_name(descriptor);
        match manager.set_descriptor_hmac(fp, alias, descriptor.to_string(), hmac) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("set_descriptor_hmac {fingerprint}: {e}")),
        }
    }

    pub fn unregister_descriptor(
        &mut self,
        fingerprint: &str,
        descriptor: &str,
    ) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        match manager.clear_descriptor_registration(fp, descriptor) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("unregister_descriptor {fingerprint}: {e}")),
        }
    }

    pub fn key_descriptors(&self, fingerprint: &str) -> Vec<ffi::KeyDescriptor> {
        let Some(manager) = self.inner.as_ref() else {
            log::error!("KeysModule::key_descriptors(): {}", self.error);
            return Vec::new();
        };
        let Ok(fp) = parse_fingerprint(fingerprint) else {
            log::error!("KeysModule::key_descriptors(): invalid fingerprint '{fingerprint}'");
            return Vec::new();
        };
        manager
            .registered_descriptors(fp)
            .iter()
            .map(|entry| ffi::KeyDescriptor {
                fingerprint: fingerprint.to_string(),
                descriptor: entry.descriptor.clone(),
                alias: entry.alias.clone(),
                registered: true,
                hmac: entry.hmac.clone().unwrap_or_default(),
            })
            .collect()
    }

    pub fn keys_for_descriptor(&self, descriptor: &str) -> Vec<ffi::KeyInfo> {
        let Some(manager) = self.inner.as_ref() else {
            log::error!("KeysModule::keys_for_descriptor(): {}", self.error);
            return Vec::new();
        };
        manager
            .keys_for_descriptor(descriptor)
            .into_iter()
            .filter_map(|fp| manager.key(fp))
            .map(|record| to_key_info(record, manager))
            .collect()
    }

    pub fn check_device_signing(
        &self,
        psbt_v2_hex: &str,
        fingerprint: &str,
        capabilities: u32,
    ) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_ref() else {
            return failed_op(&self.error);
        };
        let fingerprint = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(error) => return failed_op(&error),
        };
        let bytes = match hex::decode(psbt_v2_hex) {
            Ok(bytes) => bytes,
            Err(error) => return failed_op(&format!("invalid psbt hex: {error}")),
        };
        match manager.check_device_signing(&bytes, fingerprint, capabilities) {
            Ok(()) => ok_op(),
            Err(error) => failed_op(&error.to_string()),
        }
    }

    /// Builds the signer picker for `psbt_v2_hex`. The PSBT's own SP fields (spend
    /// derivation, tweak) name which registered key must sign first; the manager
    /// cross-checks that against every `sp(` descriptor registered on any key.
    pub fn signing_plan(&self, psbt_v2_hex: &str) -> ffi::SigningPlan {
        let Some(manager) = self.inner.as_ref() else {
            return failed_signing_plan(&self.error);
        };
        let bytes = match hex::decode(psbt_v2_hex) {
            Ok(bytes) => bytes,
            Err(e) => return failed_signing_plan(&format!("invalid psbt hex: {e}")),
        };
        match manager.signing_plan(&bytes) {
            Ok(plan) => {
                let candidates: Vec<ffi::SigningCandidate> = plan
                    .candidates
                    .iter()
                    .map(to_ffi_signing_candidate)
                    .collect();
                ffi::SigningPlan {
                    is_ok: true,
                    error: String::new(),
                    candidates,
                    sp_outputs_pending: plan.sp_outputs_pending,
                }
            }
            Err(e) => failed_signing_plan(&format!("signing_plan: {e}")),
        }
    }

    /// Copies out the record and the manager's network for the free hot-key worker
    /// functions below, so a `set_network` or route change happening after this call
    /// cannot race a worker already in flight against the copy. Fails for an unknown
    /// key or a key without a secret.
    pub fn hot_material(&self, fingerprint: &str) -> Box<HotMaterial> {
        let Some(manager) = self.inner.as_ref() else {
            return Box::new(HotMaterial {
                inner: Err(self.error.clone()),
            });
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return Box::new(HotMaterial { inner: Err(e) }),
        };
        let Some(record) = manager.key(fp) else {
            return Box::new(HotMaterial {
                inner: Err(format!("key '{fingerprint}' is not registered")),
            });
        };
        if record.secret.is_none() {
            return Box::new(HotMaterial {
                inner: Err(format!("key '{fingerprint}' has no hot secret")),
            });
        }
        Box::new(HotMaterial {
            inner: Ok((record.clone(), manager.network())),
        })
    }

    /// Commits a key `hot_seal` built into the store, adding the Hot route in the
    /// same update as `KeyManagerImpl::commit_hot_key`. Refuses a sealed key whose
    /// network does not match the manager's.
    pub fn commit_hot_key(&mut self, sealed: &SealedHotKey, alias: &str) -> ffi::KeyInfoResult {
        let (fingerprint, envelope, network) = match sealed.inner.as_ref() {
            Ok(sealed) => sealed.clone(),
            Err(e) => return failed_key_info(e),
        };
        let Some(manager) = self.inner.as_mut() else {
            return failed_key_info(&self.error);
        };
        if network != manager.network() {
            return failed_key_info("sealed key network does not match the manager's network");
        }
        if let Err(e) = manager.commit_hot_key(fingerprint, envelope, alias.to_string()) {
            return failed_key_info(&format!("commit_hot_key {alias}: {e}"));
        }
        match manager.key(fingerprint) {
            Some(record) => ok_key_info(to_key_info(record, manager)),
            None => failed_key_info("key vanished immediately after being imported"),
        }
    }

    pub fn qr_request_get_xpubs(&mut self, paths: Vec<String>) -> ffi::QrMessageResult {
        let mut parsed = Vec::with_capacity(paths.len());
        for path in &paths {
            match DerivationPath::from_str(path) {
                Ok(path) => parsed.push(path),
                Err(e) => return failed_message(&format!("invalid derivation path '{path}': {e}")),
            }
        }
        to_message(self.session.get_xpubs(parsed))
    }

    pub fn qr_request_register_descriptor(&mut self, descriptor: &str) -> ffi::QrMessageResult {
        to_message(self.session.register_descriptor(descriptor))
    }

    pub fn qr_request_verify_address(
        &mut self,
        descriptor: &str,
        path: &str,
        address: &str,
    ) -> ffi::QrMessageResult {
        let path = match DerivationPath::from_str(path) {
            Ok(path) => path,
            Err(e) => return failed_message(&format!("invalid derivation path '{path}': {e}")),
        };
        to_message(
            self.session
                .verify_address(descriptor, path, address.to_string()),
        )
    }

    pub fn qr_request_sign(
        &mut self,
        descriptors: Vec<String>,
        psbt_v2_hex: &str,
    ) -> ffi::QrMessageResult {
        let psbt = match hex::decode(psbt_v2_hex) {
            Ok(psbt) => psbt,
            Err(e) => return failed_message(&format!("invalid psbt hex: {e}")),
        };
        to_message(self.session.sign(&descriptors, &psbt))
    }

    /// The silent payment account on the module's network.
    pub fn qr_request_get_sp_account(&mut self) -> ffi::QrMessageResult {
        let Some(manager) = self.inner.as_ref() else {
            return failed_message(&self.error);
        };
        to_message(self.session.get_sp_account(manager.network()))
    }

    /// Decodes a response's bytes and validates them against the outstanding request.
    pub fn qr_decode_response(&mut self, payload: &[u8]) -> Box<QrResponse> {
        Box::new(QrResponse {
            inner: self.session.decode_response(payload),
        })
    }

    /// Builds the cached preparation request from persisted material. No disk work:
    /// the material was persisted by the worker that ran `qr_musig_prepare_material`.
    /// The box is the CXX ownership form across the boundary, not an allocation.
    #[allow(clippy::boxed_local)]
    pub fn qr_request_prepare_musig(
        &mut self,
        preparation: Box<QrMusigPrepare>,
    ) -> ffi::QrMessageResult {
        match preparation.inner {
            Ok(preparation) => to_message(self.session.prepare_musig(preparation)),
            Err(error) => failed_message(&error),
        }
    }

    /// Builds the first signature request of a prepared session. No disk work: the
    /// allocation was made durable by the worker that ran `qr_musig_sign_material`.
    /// The box is the CXX ownership form across the boundary, not an allocation.
    #[allow(clippy::boxed_local)]
    pub fn qr_request_sign_musig(&mut self, signing: Box<QrMusigSign>) -> ffi::QrMessageResult {
        match signing.inner {
            Ok(material) => to_message(self.session.sign_musig(
                material.session_id,
                material.attempt_id,
                &material.psbt,
            )),
            Err(error) => failed_message(&error),
        }
    }
}

/// A GUI-thread copy of one key's record and the manager's network, built by
/// `KeysModule::hot_material`. The free hot-key worker functions below take this
/// instead of a fingerprint, so they touch no `KeysModule` state and are safe to run
/// off the GUI thread even while the module is mutated concurrently.
pub struct HotMaterial {
    inner: Result<(KeyRecord, BtcNetwork), String>,
}

impl HotMaterial {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }
}

/// The plan route of each pending signer, keyed by fingerprint. An entry that does
/// not parse names no route.
fn plan_routes(routes: &[ffi::MusigSignerRoute]) -> BTreeMap<Fingerprint, RouteKind> {
    let mut parsed = BTreeMap::new();
    for signer in routes {
        let Ok(fingerprint) = Fingerprint::from_str(&signer.fingerprint) else {
            continue;
        };
        let Ok(route) = RouteKind::try_from(signer.route) else {
            continue;
        };
        parsed.insert(fingerprint, route);
    }
    parsed
}

/// A persisted cache preparation, or why none could be persisted. Born in the
/// worker that ran the disk work, consumed by the GUI thread's
/// `KeysModule::qr_request_prepare_musig`.
pub struct QrMusigPrepare {
    inner: Result<request::PrepareMusig, String>,
}

/// One session id, its attempt, and the transaction as the first signature request
/// must carry it: every allocated participant nonce inserted.
struct QrMusigSignMaterial {
    session_id: [u8; 32],
    attempt_id: [u8; 32],
    psbt: Vec<u8>,
}

/// A durable first-signature-request allocation, or why it failed. Born in the
/// worker that ran the disk work, consumed by the GUI thread's
/// `KeysModule::qr_request_sign_musig`.
pub struct QrMusigSign {
    inner: Result<QrMusigSignMaterial, String>,
}

/// Worker entry: persists the preparation record for a fresh QR cache session, or
/// reuses the matching in-flight one, and hands back the material the wire request
/// is built from. Disk work: never the GUI thread.
pub fn qr_musig_prepare_material(fingerprint: &str, descriptor: &str) -> Box<QrMusigPrepare> {
    let failed = |error: String| Box::new(QrMusigPrepare { inner: Err(error) });
    let Ok(fingerprint) = Fingerprint::from_str(fingerprint) else {
        return failed(format!("invalid key fingerprint: {fingerprint}"));
    };
    Box::new(QrMusigPrepare {
        inner: musig_sessions::prepare_material(fingerprint, descriptor),
    })
}

/// Worker entry: allocates every pending signer's cached session to this attempt and
/// hands back the first signature request's material. Disk work: never the GUI
/// thread. The plan names each pending signer's route; a pending signer without a
/// usable candidate fails the allocation.
pub fn qr_musig_sign_material(
    fingerprint: &str,
    policy: &str,
    psbt_v2_hex: &str,
    routes: Vec<ffi::MusigSignerRoute>,
) -> Box<QrMusigSign> {
    let failed = |error: String| Box::new(QrMusigSign { inner: Err(error) });
    let Ok(qr_fingerprint) = Fingerprint::from_str(fingerprint) else {
        return failed(format!("invalid key fingerprint: {fingerprint}"));
    };
    let Ok(bytes) = hex::decode(psbt_v2_hex) else {
        return failed("invalid psbt hex".to_string());
    };
    let (session_id, psbt) = match musig_sessions::qr_first_request(
        policy,
        &bytes,
        &plan_routes(&routes),
        qr_fingerprint,
    ) {
        Ok(allocated) => allocated,
        Err(error) => return failed(error.to_string()),
    };
    let attempt_id = match psbt::parse_v2(&psbt)
        .map_err(|e| e.to_string())
        .and_then(|parsed| musig_sessions::attempt_id(&parsed.proprietary))
    {
        Ok(Some(attempt_id)) => attempt_id,
        Ok(None) => {
            return failed("MuSig progress has no attempt identity".to_string());
        }
        Err(error) => return failed(error),
    };
    Box::new(QrMusigSign {
        inner: Ok(QrMusigSignMaterial {
            session_id,
            attempt_id,
            psbt,
        }),
    })
}

/// Worker entry: folds a decoded cached MuSig answer into the cache. The box is the
/// CXX ownership form across the boundary, not an allocation. A prepared
/// answer is checked against the policy and stored; a signing answer is checked
/// against the allocation, verified and consumed. Disk work: never the GUI thread.
#[allow(clippy::boxed_local)]
pub fn qr_musig_accept(
    answer: Box<QrResponse>,
    fingerprint: &str,
    policy: &str,
    psbt_v2_hex: &str,
) -> ffi::QrMusigCacheResult {
    let failed = |error: String, musig_session: bool| ffi::QrMusigCacheResult {
        is_ok: false,
        error,
        musig_session,
        psbt_v2_hex: String::new(),
    };
    let Ok(fingerprint) = Fingerprint::from_str(fingerprint) else {
        return failed(format!("invalid key fingerprint: {fingerprint}"), false);
    };
    let decoded = match answer.inner {
        Ok(decoded) => decoded,
        Err(error) => return failed(error.to_string(), false),
    };
    match decoded {
        qr::QrResponse::MusigPrepared {
            session_id,
            max_inputs,
            nonces,
        } => {
            let prepared = response::MusigPrepared {
                session_id,
                max_inputs,
                nonces,
            };
            match musig_sessions::accept_prepared(fingerprint, policy, prepared) {
                Ok(()) => ffi::QrMusigCacheResult {
                    is_ok: true,
                    error: String::new(),
                    musig_session: false,
                    psbt_v2_hex: String::new(),
                },
                Err(error) => failed(error, false),
            }
        }
        qr::QrResponse::MusigSignedPsbt(returned) => {
            // Only a signing answer carries the transaction it was asked with.
            let original = match hex::decode(psbt_v2_hex)
                .map_err(|e| format!("invalid psbt hex: {e}"))
                .and_then(|bytes| {
                    psbt::parse_v2(&bytes)
                        .map_err(|e| e.to_string())?
                        .into_bitcoin_psbt()
                        .map_err(|e| e.to_string())
                }) {
                Ok(original) => original,
                Err(error) => return failed(error, false),
            };
            let returned = match psbt::parse_v2(&returned)
                .map_err(|e| e.to_string())
                .and_then(|parsed| parsed.into_bitcoin_psbt().map_err(|e| e.to_string()))
            {
                Ok(returned) => returned,
                Err(error) => return failed(error, false),
            };
            match musig_sessions::accept_signed(fingerprint, policy, &original, &returned) {
                Ok(()) => {
                    let serialized = bwk::bwk_tx::bwk_psbt::PsbtV2::from_bitcoin_psbt(returned)
                        .map_err(|e| e.to_string())
                        .and_then(|parsed| parsed.serialize().map_err(|e| e.to_string()));
                    match serialized {
                        Ok(bytes) => ffi::QrMusigCacheResult {
                            is_ok: true,
                            error: String::new(),
                            musig_session: false,
                            psbt_v2_hex: hex::encode(bytes),
                        },
                        Err(error) => {
                            failed(format!("cannot serialize the signed psbt: {error}"), false)
                        }
                    }
                }
                Err(error) => failed(
                    error.to_string(),
                    matches!(error, musig_sessions::SigningError::SessionLost(_)),
                ),
            }
        }
        _ => failed("expected a cached MuSig answer".to_string(), false),
    }
}

/// A mnemonic sealed by the free `hot_seal`, or the error that refused it. Committed
/// to the store by `KeysModule::commit_hot_key` on the GUI thread.
pub struct SealedHotKey {
    inner: Result<(Fingerprint, SecretEnvelope, BtcNetwork), String>,
}

impl SealedHotKey {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }

    /// Empty on error.
    pub fn fingerprint(&self) -> String {
        match self.inner.as_ref() {
            Ok((fingerprint, ..)) => fingerprint.to_string(),
            Err(_) => String::new(),
        }
    }
}

/// Derives the fingerprint and seals `mnemonic` under `password` for `network`,
/// touching no `KeysModule` state. Blocks on Argon2id: call from a QThread, never
/// from the GUI thread.
pub fn hot_seal(mnemonic: &str, password: &str, network: ffi::Network) -> Box<SealedHotKey> {
    let network = BtcNetwork::from(network);
    let mnemonic = Zeroizing::new(mnemonic.to_string());
    let password = Password::new(password.to_string());
    let inner = seal_mnemonic(&mnemonic, network, password)
        .map(|(fingerprint, envelope)| (fingerprint, envelope, network))
        .map_err(|e| e.to_string());
    Box::new(SealedHotKey { inner })
}

/// Blocks on Argon2id while opening the envelope: call from a QThread, never from the
/// GUI thread.
pub fn hot_reveal(material: &HotMaterial, password: &str) -> ffi::MnemonicResult {
    let password = Password::new(password.to_string());
    let (record, network) = match material.inner.as_ref() {
        Ok(pair) => pair,
        Err(e) => return failed_mnemonic(e),
    };
    match open_mnemonic(record, *network, password) {
        Ok(mnemonic) => ffi::MnemonicResult {
            is_ok: true,
            error: String::new(),
            mnemonic: mnemonic.to_string(),
            wrong_password: false,
        },
        Err(e) => ffi::MnemonicResult {
            wrong_password: matches!(e, Error::WrongPassword),
            ..failed_mnemonic(&format!("reveal {}: {e}", record.fingerprint))
        },
    }
}

/// Signs `psbt_v2_hex` with `material`'s hot secret and returns the signed PSBT as v2
/// hex. Blocks on Argon2id while opening the envelope: call from a QThread, never
/// from the GUI thread.
pub fn hot_sign(
    material: &HotMaterial,
    password: &str,
    psbt_v2_hex: &str,
    routes: Vec<ffi::MusigSignerRoute>,
) -> ffi::SignResult {
    let password = Password::new(password.to_string());
    let (record, network) = match material.inner.as_ref() {
        Ok(pair) => pair,
        Err(e) => return failed_sign(e),
    };
    let psbt_bytes = match hex::decode(psbt_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_sign(&format!("invalid psbt hex: {e}")),
    };
    match sign_hot(
        record,
        *network,
        password,
        &psbt_bytes,
        &plan_routes(&routes),
    ) {
        Ok(signed) => ffi::SignResult {
            is_ok: true,
            error: String::new(),
            wrong_password: false,
            musig_session: false,
            psbt_v2_hex: hex::encode(signed),
        },
        Err(e) => ffi::SignResult {
            wrong_password: matches!(e, Error::WrongPassword),
            musig_session: matches!(e, Error::MusigSession),
            ..failed_sign(&format!("sign_hot {}: {e}", record.fingerprint))
        },
    }
}

pub fn account_xpub_usage(network: ffi::Network) -> Box<AccountXpubUsage> {
    let network = BtcNetwork::from(network);
    let mut usage = AccountXpubUsage {
        network,
        used: Vec::new(),
        error: String::new(),
    };
    let mut manager = match open_manager(network.into()) {
        Ok(manager) => manager,
        Err(error) => {
            usage.error = error;
            return Box::new(usage);
        }
    };
    for name in list_configs() {
        let config = match Config::from_file(name.clone()) {
            Ok(config) => config,
            Err(e) => {
                log::error!("account_descriptors: account '{name}': {e}");
                continue;
            }
        };
        if BtcNetwork::from(config.get_network()) != network {
            continue;
        }
        for descriptor in [
            config.get_descriptor(),
            config.get_segwit_descriptor(),
            config.get_taproot_descriptor(),
        ] {
            if let Err(error) = manager.mark_xpubs_used(&descriptor) {
                usage.error = format!("mark account '{name}' xpub usage: {error}");
            }
        }
    }
    usage.used = manager.used_xpubs();
    Box::new(usage)
}

pub fn hot_create_sessions(
    material: &HotMaterial,
    password: &str,
    policy: &str,
    quantity: u32,
) -> ffi::MusigSessionCreateResult {
    let result = match &material.inner {
        Ok((record, network)) => create_sessions(
            record,
            *network,
            Password::new(password.to_owned()),
            policy,
            quantity as usize,
        ),
        Err(error) => {
            return ffi::MusigSessionCreateResult {
                is_ok: false,
                error: error.clone(),
                wrong_password: false,
            }
        }
    };
    match result {
        Ok(()) => ffi::MusigSessionCreateResult {
            is_ok: true,
            error: String::new(),
            wrong_password: false,
        },
        Err(error) => ffi::MusigSessionCreateResult {
            is_ok: false,
            wrong_password: matches!(error, Error::WrongPassword),
            error: error.to_string(),
        },
    }
}

pub fn hot_cached_nonces(
    material: &HotMaterial,
    psbt_v2_hex: &str,
    routes: Vec<ffi::MusigSignerRoute>,
) -> ffi::HotCachedNonceResult {
    let mut response = ffi::HotCachedNonceResult {
        is_ok: false,
        error: String::new(),
        needs_password: false,
        musig_session: false,
        psbt_v2_hex: String::new(),
    };
    let result = (|| {
        let (record, _) = material
            .inner
            .as_ref()
            .map_err(|error| Error::State(error.clone()))?;
        let bytes = hex::decode(psbt_v2_hex).map_err(|error| Error::State(error.to_string()))?;
        publish_cached(record, &bytes, &plan_routes(&routes))
    })();
    match result {
        Ok(CachedPublication::Nonces(bytes)) => {
            response.is_ok = true;
            response.psbt_v2_hex = hex::encode(bytes);
        }
        Ok(CachedPublication::FirstRequest(bytes)) => {
            response.is_ok = true;
            response.needs_password = true;
            response.psbt_v2_hex = hex::encode(bytes);
        }
        Ok(CachedPublication::NeedsPassword) => {
            response.is_ok = true;
            response.needs_password = true;
        }
        Err(error) => {
            response.musig_session = matches!(error, Error::MusigSession);
            response.error = error.to_string();
        }
    }
    response
}

pub fn musig_session_refresh(fingerprint: &str) -> ffi::KeyOpResult {
    ledger_nonce_refresh(fingerprint)
}

pub fn musig_session_count(fingerprint: &str, policy: &str, provider: ffi::KeyRouteKind) -> u32 {
    match parse_fingerprint(fingerprint)
        .and_then(|fp| RouteKind::try_from(provider).map(|provider| (fp, provider)))
    {
        Ok((fp, provider)) => musig_sessions::count_for(fp, policy, provider) as u32,
        Err(error) => {
            log::error!("{error}");
            0
        }
    }
}

pub fn musig_session_limit() -> u32 {
    musig_sessions::MAX_SESSIONS as u32
}

pub fn musig_session_plan(
    network: ffi::Network,
    psbt_v2_hex: &str,
    devices: Vec<ffi::LiveDevice>,
) -> ffi::SigningPlan {
    ledger_nonce_plan(network, psbt_v2_hex, devices)
}

pub fn musig_session_restart(
    network: ffi::Network,
    policy: &str,
    original: &str,
    prepared: &str,
) -> ffi::SignResult {
    ledger_nonce_restart(network, policy, original, prepared)
}

/// The BIP392 sp() descriptor of `material`'s hot key, origin included, for account
/// creation. Blocks on Argon2id while opening the envelope: call from a QThread,
/// never from the GUI thread.
pub fn hot_sp_descriptor(material: &HotMaterial, password: &str) -> ffi::HotSpDescriptorResult {
    let password = Password::new(password.to_string());
    let (record, network) = match material.inner.as_ref() {
        Ok(pair) => pair,
        Err(e) => return failed_hot_sp_descriptor(e),
    };
    match sp_descriptor(record, *network, password) {
        Ok(descriptor) => ffi::HotSpDescriptorResult {
            is_ok: true,
            error: String::new(),
            wrong_password: false,
            descriptor: descriptor.to_string(),
        },
        Err(e) => ffi::HotSpDescriptorResult {
            wrong_password: matches!(e, Error::WrongPassword),
            ..failed_hot_sp_descriptor(&format!("hot_sp_descriptor {}: {e}", record.fingerprint))
        },
    }
}

/// The xpub at `path` of `material`'s hot secret.
pub fn hot_xpub(material: &HotMaterial, password: &str, path: &str) -> ffi::HotXpubResult {
    let password = Password::new(password.to_string());
    let (record, network) = match material.inner.as_ref() {
        Ok(pair) => pair,
        Err(e) => return failed_hot_xpub(e),
    };
    let path = match DerivationPath::from_str(path) {
        Ok(path) => path,
        Err(e) => return failed_hot_xpub(&format!("invalid derivation path '{path}': {e}")),
    };
    match xpub_at(record, *network, password, &path) {
        Ok(xpub) => ffi::HotXpubResult {
            is_ok: true,
            error: String::new(),
            wrong_password: false,
            xpub: xpub.to_string(),
        },
        Err(e) => ffi::HotXpubResult {
            wrong_password: matches!(e, Error::WrongPassword),
            ..failed_hot_xpub(&format!("hot_xpub {}: {e}", record.fingerprint))
        },
    }
}

/// The key `fingerprint` of a registration of `descriptor`, refused when the
/// descriptor does not parse or its key origins do not name the key.
fn registration_key(fingerprint: &str, descriptor: &str) -> Result<Fingerprint, String> {
    let fp = parse_fingerprint(fingerprint)?;
    let parsed =
        Descriptor::from_str(descriptor).map_err(|e| format!("invalid descriptor: {e}"))?;
    // `Descriptor::fingerprint()` returns only one arbitrary key's origin (the
    // first one miniscript's traversal visits), so it cannot be used to check
    // membership in a multi-key (e.g. multisig) descriptor. For the Miniscript
    // variant, walk every key origin instead; SP descriptors are inherently
    // single-key so `fingerprint()` is still exact there.
    if !descriptor_has_key(&parsed, fp) {
        return Err(format!(
            "descriptor's key origins do not include key '{fingerprint}'"
        ));
    }
    Ok(fp)
}

/// Converts `psbt_v2_hex` into its v0 base64 payload for a USB signing device.
/// Cheap; safe to call from the GUI thread. Refuses a PSBT with a pending SP
/// output: a signing device only ever signs its own bip32 inputs, and the signing
/// plan already blocks every other candidate while one is pending. In MuSig2 round 1
/// every nonce and partial signature is dropped, as the QR route does, because a
/// device takes any nonce in the PSBT as round 2 (Ledger's rule); `usb_merge_signed`
/// merges into the unstripped original, so the nonces already gathered stay.
pub fn usb_sign_payload(
    psbt_v2_hex: &str,
    descriptor: &str,
    open_musig_session: bool,
) -> ffi::PsbtPayloadResult {
    let bytes = match hex::decode(psbt_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_psbt_payload(&format!("invalid psbt hex: {e}")),
    };
    let mut parsed = match psbt::parse_v2(&bytes) {
        Ok(parsed) => parsed,
        Err(e) => return failed_psbt_payload(&e.to_string()),
    };
    if let Err(e) = psbt::strip_musig_round1(&mut parsed) {
        return failed_psbt_payload(&e.to_string());
    }
    if open_musig_session {
        if let Some((aggregate, participants)) = musig_internal_key(descriptor) {
            if let Err(e) = psbt::add_internal_participants(&mut parsed, aggregate, &participants) {
                return failed_psbt_payload(&e.to_string());
            }
        }
    }
    let v0 = match parsed.into_bitcoin_psbt() {
        Ok(v0) => v0,
        Err(PsbtV2Error::MissingOutputScript) => {
            return failed_psbt_payload(&Error::SpOutputsPending.to_string())
        }
        Err(e) => return failed_psbt_payload(&Error::PsbtParse(e.to_string()).to_string()),
    };
    ffi::PsbtPayloadResult {
        is_ok: true,
        error: String::new(),
        psbt_base64: BASE64_STANDARD.encode(v0.serialize()),
    }
}

/// The aggregate and KeySort participants of `descriptor`'s internal key when it is a
/// musig() of plain (unranged) cosigners, None otherwise.
fn musig_internal_key(
    descriptor: &str,
) -> Option<(
    bitcoin::secp256k1::PublicKey,
    Vec<bitcoin::secp256k1::PublicKey>,
)> {
    let parsed = parse_musig_descriptor(descriptor).ok()?;
    let miniscript::Descriptor::Tr(tr) = parsed else {
        return None;
    };
    let MusigKey::Musig(aggregate) = tr.internal_key() else {
        return None;
    };
    Some((aggregate.aggregate()?, aggregate.participants().to_vec()))
}

/// Merges a USB device's v0 signed answer back into `original_v2_hex`, keeping its
/// silent-payment input tweaks and ECDH shares. Only signature fields are copied.
/// Cheap; safe to call from the GUI thread.
pub fn usb_merge_signed(original_v2_hex: &str, signed_v0_base64: &str) -> ffi::SignResult {
    let original_bytes = match hex::decode(original_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_sign(&format!("invalid psbt hex: {e}")),
    };
    let signed_bytes = match BASE64_STANDARD.decode(signed_v0_base64.trim()) {
        Ok(bytes) => bytes,
        Err(e) => return failed_sign(&format!("invalid base64: {e}")),
    };
    match psbt::merge_signatures_bytes(&original_bytes, &signed_bytes) {
        Ok(merged) => ffi::SignResult {
            is_ok: true,
            error: String::new(),
            wrong_password: false,
            musig_session: false,
            psbt_v2_hex: hex::encode(merged),
        },
        Err(e) => failed_sign(&e.to_string()),
    }
}

/// See the bridge declaration.
pub fn merge_signed_psbt(current_v2_hex: &str, signed_v2_hex: &str) -> ffi::SignResult {
    let current = match hex::decode(current_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_sign(&format!("invalid psbt hex: {e}")),
    };
    let signed = match hex::decode(signed_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_sign(&format!("invalid psbt hex: {e}")),
    };
    let merged = match psbt::merge_signatures_bytes(&current, &signed) {
        Ok(merged) => hex::encode(merged),
        // Already in: another answer brought the same.
        Err(Error::NothingSigned) => current_v2_hex.to_string(),
        Err(e) => return failed_sign(&e.to_string()),
    };
    ffi::SignResult {
        is_ok: true,
        error: String::new(),
        wrong_password: false,
        musig_session: false,
        psbt_v2_hex: merged,
    }
}

/// Wraps a decoded and validated QR response, or the error that rejected it. An
/// accessor for a kind the response is not returns an empty value.
pub struct QrResponse {
    inner: Result<qr::QrResponse, Error>,
}

impl QrResponse {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner
            .as_ref()
            .err()
            .map(ToString::to_string)
            .unwrap_or_default()
    }

    /// Only meaningful when !is_ok(). `Other` is an arbitrary sentinel on success.
    pub fn error_kind(&self) -> ffi::QrErrorKind {
        self.inner
            .as_ref()
            .err()
            .map_or(ffi::QrErrorKind::Other, ffi::QrErrorKind::from)
    }

    /// Only meaningful when is_ok(). `Xpubs` is an arbitrary sentinel on error.
    pub fn kind(&self) -> ffi::QrResponseKind {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { .. }) | Err(_) => ffi::QrResponseKind::Xpubs,
            Ok(qr::QrResponse::Registration { .. }) => ffi::QrResponseKind::Registration,
            Ok(qr::QrResponse::AddressVerified(_)) => ffi::QrResponseKind::AddressVerified,
            Ok(qr::QrResponse::SignedPsbt(_)) => ffi::QrResponseKind::SignedPsbt,
            Ok(qr::QrResponse::SpAccount { .. }) => ffi::QrResponseKind::SpAccount,
            Ok(qr::QrResponse::MusigPrepared { .. }) => ffi::QrResponseKind::MusigPrepared,
            Ok(qr::QrResponse::MusigSignedPsbt(_)) => ffi::QrResponseKind::MusigSignedPsbt,
        }
    }

    /// The signer's fingerprint when kind() is Xpubs or SpAccount, else empty.
    pub fn fingerprint(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { fingerprint, .. })
            | Ok(qr::QrResponse::SpAccount { fingerprint, .. }) => fingerprint.to_string(),
            _ => String::new(),
        }
    }

    /// The SP descriptor when kind() is SpAccount, else empty.
    pub fn descriptor(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::SpAccount { descriptor, .. }) => descriptor.clone(),
            _ => String::new(),
        }
    }

    /// The original v2 PSBT with the device's signatures merged in, as hex, when
    /// kind() is SignedPsbt, else empty.
    pub fn psbt_hex(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::SignedPsbt(bytes)) => hex::encode(bytes),
            _ => String::new(),
        }
    }

    /// The device vendor when kind() is Xpubs, else empty.
    pub fn vendor(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { vendor, .. }) => vendor.clone(),
            _ => String::new(),
        }
    }

    /// The device model when kind() is Xpubs, else empty.
    pub fn model(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { model, .. }) => model.clone(),
            _ => String::new(),
        }
    }

    /// The device firmware version when kind() is Xpubs, else empty.
    pub fn version(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { version, .. }) => version.clone(),
            _ => String::new(),
        }
    }

    /// The device capability bits when kind() is Xpubs, else 0.
    pub fn capabilities(&self) -> u32 {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { capabilities, .. }) => *capabilities,
            _ => 0,
        }
    }

    /// The derived xpubs when kind() is Xpubs, the taproot and segwit xpubs when
    /// SpAccount, else empty.
    pub fn xpubs(&self) -> Vec<ffi::KeyXpub> {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs {
                fingerprint, xpubs, ..
            })
            | Ok(qr::QrResponse::SpAccount {
                fingerprint, xpubs, ..
            }) => xpubs
                .iter()
                .map(|(path, xpub)| ffi::KeyXpub {
                    fingerprint: fingerprint.to_string(),
                    path: path.to_string(),
                    xpub: xpub.to_string(),
                    // Answered by the signer, not cached yet.
                    used: false,
                })
                .collect(),
            _ => Vec::new(),
        }
    }

    /// The device's verify_address answer when kind() is AddressVerified, else false.
    pub fn verified(&self) -> bool {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::AddressVerified(matches)) => *matches,
            _ => false,
        }
    }

    pub fn registered(&self) -> bool {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Registration { registered, .. }) => *registered,
            _ => false,
        }
    }
}

fn to_message(request: Result<qr::QrRequest, Error>) -> ffi::QrMessageResult {
    match request {
        Ok(request) => ffi::QrMessageResult {
            is_ok: true,
            error: String::new(),
            payload: request.into_message(),
        },
        Err(e) => failed_message(&e.to_string()),
    }
}

fn failed_message(error: &str) -> ffi::QrMessageResult {
    ffi::QrMessageResult {
        is_ok: false,
        error: error.to_string(),
        payload: Vec::new(),
    }
}

/// Parsed once here: every other function in this module takes the fingerprint as a
/// lowercase hex string and calls this first.
fn parse_fingerprint(value: &str) -> Result<Fingerprint, String> {
    Fingerprint::from_hex(value).map_err(|e| format!("invalid fingerprint '{value}': {e}"))
}

pub struct LedgerNonceFetch {
    inner: Result<Option<Fetch>, String>,
}

pub fn ledger_nonce_fetch(fingerprint: &str, policy: &str, quantity: u32) -> Box<LedgerNonceFetch> {
    Box::new(LedgerNonceFetch {
        inner: parse_fingerprint(fingerprint)
            .and_then(|fp| Fetch::begin(fp, policy, quantity as usize).map(Some)),
    })
}

pub fn ledger_nonce_fetch_for_signing(
    fingerprint: &str,
    policy: &str,
    psbt_v2_hex: &str,
) -> Box<LedgerNonceFetch> {
    let inner = (|| {
        let fp = parse_fingerprint(fingerprint)?;
        let bytes = hex::decode(psbt_v2_hex).map_err(|e| e.to_string())?;
        Fetch::for_signing(fp, policy, &bytes)
    })();
    Box::new(LedgerNonceFetch { inner })
}

impl LedgerNonceFetch {
    pub fn payload(&self) -> ffi::PsbtPayloadResult {
        match &self.inner {
            Ok(Some(fetch)) => ffi::PsbtPayloadResult {
                is_ok: true,
                error: String::new(),
                psbt_base64: BASE64_STANDARD.encode(fetch.payload().serialize()),
            },
            Ok(None) => ffi::PsbtPayloadResult {
                is_ok: true,
                error: String::new(),
                psbt_base64: String::new(),
            },
            Err(error) => failed_psbt_payload(error),
        }
    }

    // An empty payload means the batch reached disk or a signing fetch was unnecessary.
    pub fn accept(&mut self, signed_psbt: &str) -> ffi::PsbtPayloadResult {
        let result: Result<String, String> = (|| {
            let signed = bitcoin::Psbt::from_str(signed_psbt).map_err(|e| e.to_string())?;
            let fetch = self
                .inner
                .as_mut()
                .map_err(|e| e.clone())?
                .as_mut()
                .ok_or("no Ledger nonce fetch is in progress")?;
            let done = fetch.accept(&signed)?;
            Ok(if done {
                String::new()
            } else {
                BASE64_STANDARD.encode(fetch.payload().serialize())
            })
        })();
        match result {
            Ok(psbt_base64) => ffi::PsbtPayloadResult {
                is_ok: true,
                error: String::new(),
                psbt_base64,
            },
            Err(error) => {
                self.inner = Err(error.clone());
                failed_psbt_payload(&error)
            }
        }
    }
}

pub fn ledger_nonce_refresh(fingerprint: &str) -> ffi::KeyOpResult {
    match parse_fingerprint(fingerprint).and_then(ledger_nonces::refresh) {
        Ok(()) => ok_op(),
        Err(error) => failed_op(&error),
    }
}

pub fn ledger_nonce_count(fingerprint: &str, policy: &str) -> u32 {
    match parse_fingerprint(fingerprint) {
        Ok(fp) => ledger_nonces::count(fp, policy) as u32,
        Err(error) => {
            log::error!("{error}");
            0
        }
    }
}

pub fn ledger_nonce_limit() -> u32 {
    ledger_nonces::MAX_LEDGER_NONCE_SESSIONS as u32
}

pub fn ledger_nonce_plan(
    network: ffi::Network,
    psbt_v2_hex: &str,
    devices: Vec<ffi::LiveDevice>,
) -> ffi::SigningPlan {
    let result: Result<ffi::SigningPlan, String> = (|| {
        let mut module = build_module(network);
        if !module.is_ok() {
            return Err(module.get_error());
        }
        module.set_live_devices(devices);
        for record in module
            .inner
            .as_ref()
            .ok_or("keys module is unavailable")?
            .keys()
        {
            // A cache that cannot be read leaves the last snapshot in place: the
            // plan stays usable, readiness just reflects what is already loaded.
            if let Err(error) = ledger_nonces::ensure_snapshot(record.fingerprint) {
                log::error!("MuSig session cache: {error}");
            }
        }
        Ok(module.signing_plan(psbt_v2_hex))
    })();
    match result {
        Ok(plan) => plan,
        Err(error) => failed_signing_plan(&error),
    }
}

pub fn ledger_nonce_sign(
    fingerprint: &str,
    policy: &str,
    psbt_v2_hex: &str,
    routes: Vec<ffi::MusigSignerRoute>,
) -> ffi::LedgerNonceSignResult {
    let mut response = ffi::LedgerNonceSignResult {
        is_ok: false,
        error: String::new(),
        musig_session: false,
        need_fetch: false,
        musig: false,
        from_cache: false,
        psbt_v2_hex: String::new(),
        psbt_base64: String::new(),
        request_v2_hex: String::new(),
    };
    let result: Result<(), ledger_nonces::SigningError> = (|| {
        let fp = parse_fingerprint(fingerprint)?;
        let bytes = hex::decode(psbt_v2_hex).map_err(|e| e.to_string())?;
        let parsed = psbt::parse_v2(&bytes).map_err(|e| e.to_string())?;
        let descriptor = ledger_nonces::descriptor(policy)?;
        let real = parsed.into_bitcoin_psbt().map_err(|e| e.to_string())?;
        let owned = descriptor.has_musig()
            && !own_slots(&real, &descriptor, fp)
                .map_err(|e| e.to_string())?
                .is_empty();
        if !owned {
            if ledger_nonces::requested_by(&real, fp)? {
                return Err("policy does not cover the requested Ledger MuSig inputs".into());
            }
            let payload = usb_sign_payload(psbt_v2_hex, policy, false);
            if !payload.is_ok {
                return Err(payload.error.into());
            }
            response.psbt_base64 = payload.psbt_base64;
            return Ok(());
        }
        response.musig = true;
        match ledger_nonces::prepare_request(fp, policy, &bytes, &plan_routes(&routes))? {
            Signing::Nonces { bytes, source } => {
                response.psbt_v2_hex = hex::encode(bytes);
                response.from_cache = source == SessionSource::Cached;
            }
            Signing::Missing => response.need_fetch = true,
            Signing::Payload {
                psbt,
                source,
                request,
            } => {
                response.psbt_base64 = BASE64_STANDARD.encode(psbt.serialize());
                response.request_v2_hex = hex::encode(request);
                response.from_cache = source == SessionSource::Cached;
            }
        }
        Ok(())
    })();
    match result {
        Ok(()) => response.is_ok = true,
        Err(error) => {
            response.musig_session = matches!(error, ledger_nonces::SigningError::SessionLost(_));
            response.error = error.to_string();
        }
    }
    response
}

pub fn ledger_nonce_merge(
    fingerprint: &str,
    policy: &str,
    original: &str,
    signed_psbt: &str,
) -> ffi::SignResult {
    let merged = usb_merge_signed(original, signed_psbt);
    if !merged.is_ok {
        return merged;
    }
    let checked: Result<(), ledger_nonces::SigningError> = (|| {
        let fp = parse_fingerprint(fingerprint)?;
        let descriptor = ledger_nonces::descriptor(policy)?;
        let bytes = hex::decode(&merged.psbt_v2_hex).map_err(|e| e.to_string())?;
        let real = psbt::parse_v2(&bytes)
            .map_err(|e| e.to_string())?
            .into_bitcoin_psbt()
            .map_err(|e| e.to_string())?;
        ledger_nonces::check_round2(&real, &descriptor, fp)?;
        let original = hex::decode(original).map_err(|e| e.to_string())?;
        ledger_nonces::finish(fp, policy, &original)
    })();
    match checked {
        Ok(()) => merged,
        Err(error) => failed_ledger_sign(error),
    }
}

fn failed_ledger_sign(error: ledger_nonces::SigningError) -> ffi::SignResult {
    ffi::SignResult {
        musig_session: matches!(error, ledger_nonces::SigningError::SessionLost(_)),
        ..failed_sign(&error.to_string())
    }
}

pub fn ledger_nonce_invalidate(
    fingerprint: &str,
    policy: &str,
    psbt_v2_hex: &str,
) -> ffi::SignResult {
    let result: Result<(), ledger_nonces::SigningError> = (|| {
        let fp = parse_fingerprint(fingerprint)?;
        let bytes = hex::decode(psbt_v2_hex).map_err(|e| e.to_string())?;
        ledger_nonces::invalidate_signing(fp, policy, &bytes)
    })();
    match result {
        Ok(()) => ffi::SignResult {
            is_ok: true,
            ..failed_sign("")
        },
        Err(error) => failed_ledger_sign(error),
    }
}

pub fn ledger_nonce_restart(
    network: ffi::Network,
    policy: &str,
    original: &str,
    prepared: &str,
) -> ffi::SignResult {
    let result: Result<Vec<u8>, String> = (|| {
        let manager = open_manager(network)?;
        let registered: BTreeSet<_> = manager
            .keys()
            .iter()
            .filter(|record| {
                record.secret.is_some()
                    || record
                        .devices
                        .iter()
                        .any(|device| ledger_nonces::is_ledger(&device.kind))
            })
            .map(|record| record.fingerprint)
            .collect();
        let descriptor = ledger_nonces::descriptor(policy)?;
        let named: BTreeSet<_> = descriptor
            .plain_keys()
            .iter()
            .map(|key| key.master_fingerprint())
            .collect();
        let mut fingerprints = Vec::new();
        for fingerprint in named {
            if registered.contains(&fingerprint) || ledger_nonces::has_cache(fingerprint)? {
                fingerprints.push(fingerprint);
            }
        }
        let original = hex::decode(original).map_err(|e| e.to_string())?;
        let prepared = hex::decode(prepared).map_err(|e| e.to_string())?;
        ledger_nonces::restart(policy, &original, &prepared, &fingerprints)
    })();
    match result {
        Ok(bytes) => ffi::SignResult {
            is_ok: true,
            psbt_v2_hex: hex::encode(bytes),
            ..failed_sign("")
        },
        Err(error) => failed_sign(&error),
    }
}

/// True when `child` is exactly one derivation step below `parent`.
/// The key origin's fingerprint, if any. Mirrors the per-variant match in
/// `Descriptor::fingerprint()`, but is applied per-key via `for_any_key` instead
/// of stopping at the first key that has an origin.
fn to_key_info(record: &KeyRecord, manager: &KeyManager) -> ffi::KeyInfo {
    ffi::KeyInfo {
        fingerprint: record.fingerprint.to_string(),
        alias: record.alias.clone(),
        has_hot_secret: record.secret.is_some(),
        routes: record
            .routes
            .as_slice()
            .iter()
            .copied()
            .map(ffi::KeyRouteKind::from)
            .collect(),
        is_connected: manager.is_connected(record.fingerprint),
        device_count: record.devices.len() as u32,
        descriptor_count: record.descriptors.len() as u32,
        can_musig: manager.can_musig(record.fingerprint),
    }
}

fn to_manager_live_device(device: ffi::LiveDevice) -> LiveDevice {
    // An empty fingerprint is expected for a locked or unsupported device that
    // has not reported one yet; only a non-empty, unparseable value is an error.
    let fingerprint = if device.fingerprint.is_empty() {
        None
    } else {
        match parse_fingerprint(&device.fingerprint) {
            Ok(fp) => Some(fp),
            Err(e) => {
                log::error!("to_manager_live_device(): {e}");
                None
            }
        }
    };
    LiveDevice {
        id: device.device_id,
        kind: device.kind,
        model: device.model,
        serial: device.serial,
        module: device.module,
        version: device.version,
        capabilities: device.capabilities,
        fingerprint,
    }
}

fn to_ffi_live_device(device: &LiveDevice) -> ffi::LiveDevice {
    ffi::LiveDevice {
        device_id: device.id.clone(),
        fingerprint: device
            .fingerprint
            .map(|fp| fp.to_string())
            .unwrap_or_default(),
        kind: device.kind.clone(),
        // manager::LiveDevice carries no brand field; bwk-hwi has not reported one.
        brand: String::new(),
        model: device.model.clone(),
        serial: device.serial.clone(),
        module: device.module.clone(),
        version: device.version.clone(),
        capabilities: device.capabilities,
    }
}

fn ok_op() -> ffi::KeyOpResult {
    ffi::KeyOpResult {
        is_ok: true,
        error: String::new(),
    }
}

fn failed_op(error: &str) -> ffi::KeyOpResult {
    ffi::KeyOpResult {
        is_ok: false,
        error: error.to_string(),
    }
}

fn ok_key_info(key: ffi::KeyInfo) -> ffi::KeyInfoResult {
    ffi::KeyInfoResult {
        is_ok: true,
        error: String::new(),
        key,
    }
}

fn failed_key_info(error: &str) -> ffi::KeyInfoResult {
    ffi::KeyInfoResult {
        is_ok: false,
        error: error.to_string(),
        key: empty_key_info(),
    }
}

fn empty_key_info() -> ffi::KeyInfo {
    ffi::KeyInfo {
        fingerprint: String::new(),
        alias: String::new(),
        has_hot_secret: false,
        routes: Vec::new(),
        is_connected: false,
        device_count: 0,
        descriptor_count: 0,
        can_musig: false,
    }
}

fn failed_signing_plan(error: &str) -> ffi::SigningPlan {
    ffi::SigningPlan {
        is_ok: false,
        error: error.to_string(),
        candidates: Vec::new(),
        sp_outputs_pending: false,
    }
}

fn failed_mnemonic(error: &str) -> ffi::MnemonicResult {
    ffi::MnemonicResult {
        is_ok: false,
        error: error.to_string(),
        mnemonic: String::new(),
        wrong_password: false,
    }
}

fn failed_sign(error: &str) -> ffi::SignResult {
    ffi::SignResult {
        is_ok: false,
        error: error.to_string(),
        wrong_password: false,
        musig_session: false,
        psbt_v2_hex: String::new(),
    }
}

fn failed_psbt_payload(error: &str) -> ffi::PsbtPayloadResult {
    ffi::PsbtPayloadResult {
        is_ok: false,
        error: error.to_string(),
        psbt_base64: String::new(),
    }
}

fn failed_hot_sp_descriptor(error: &str) -> ffi::HotSpDescriptorResult {
    ffi::HotSpDescriptorResult {
        is_ok: false,
        error: error.to_string(),
        wrong_password: false,
        descriptor: String::new(),
    }
}

fn failed_hot_xpub(error: &str) -> ffi::HotXpubResult {
    ffi::HotXpubResult {
        is_ok: false,
        error: error.to_string(),
        wrong_password: false,
        xpub: String::new(),
    }
}

/// The manager only ever builds a candidate for a fingerprint the PSBT needs.
fn to_ffi_signing_candidate(candidate: &SigningCandidate) -> ffi::SigningCandidate {
    ffi::SigningCandidate {
        fingerprint: candidate.fingerprint.to_string(),
        alias: candidate.alias.clone(),
        route: candidate
            .route
            .map(ffi::KeyRouteKind::from)
            .unwrap_or(ffi::KeyRouteKind::Unknown),
        device_id: candidate.device_id.clone(),
        can_sign: candidate.unusable.is_none(),
        unusable: ffi::KeyUnusableReason::from(candidate.unusable),
        must_sign_first: candidate.must_sign_first,
        registered: candidate.registered,
        musig_done: candidate.musig_done,
        nonces_done: candidate.nonces_done,
        signatures_done: candidate.signatures_done,
        cache_ready: candidate.cache_ready,
        cache_supported: candidate.cache_supported,
    }
}

impl TryFrom<ffi::KeyRouteKind> for RouteKind {
    type Error = String;

    /// `Unknown` is only ever produced for a `SigningCandidate`; `add_route`,
    /// `remove_route` and `set_route_order` reject it instead of panicking, since
    /// C++ input reaches this conversion.
    fn try_from(kind: ffi::KeyRouteKind) -> Result<Self, Self::Error> {
        match kind {
            ffi::KeyRouteKind::Hot => Ok(RouteKind::Hot),
            ffi::KeyRouteKind::Usb => Ok(RouteKind::Usb),
            ffi::KeyRouteKind::Qr => Ok(RouteKind::Qr),
            ffi::KeyRouteKind::SdCard => Ok(RouteKind::SdCard),
            ffi::KeyRouteKind::Unknown => Err("unknown route kind".to_string()),
            _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
        }
    }
}

impl From<RouteKind> for ffi::KeyRouteKind {
    fn from(kind: RouteKind) -> Self {
        match kind {
            RouteKind::Hot => ffi::KeyRouteKind::Hot,
            RouteKind::Usb => ffi::KeyRouteKind::Usb,
            RouteKind::Qr => ffi::KeyRouteKind::Qr,
            RouteKind::SdCard => ffi::KeyRouteKind::SdCard,
        }
    }
}

impl From<ffi::KeyUnusableReason> for Option<crate::keys::manager::UnusableReason> {
    fn from(reason: ffi::KeyUnusableReason) -> Self {
        use crate::keys::manager::UnusableReason;
        match reason {
            ffi::KeyUnusableReason::None => None,
            ffi::KeyUnusableReason::NotRegistered => Some(UnusableReason::NotRegistered),
            ffi::KeyUnusableReason::NoRoute => Some(UnusableReason::NoRoute),
            ffi::KeyUnusableReason::DeviceNotConnected => Some(UnusableReason::DeviceNotConnected),
            ffi::KeyUnusableReason::NoHotSecret => Some(UnusableReason::NoHotSecret),
            ffi::KeyUnusableReason::SpOutputsPending => Some(UnusableReason::SpOutputsPending),
            ffi::KeyUnusableReason::UnsupportedMusig => Some(UnusableReason::UnsupportedMusig),
            ffi::KeyUnusableReason::UnsupportedOperation => {
                Some(UnusableReason::UnsupportedOperation)
            }
            _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
        }
    }
}

impl From<Option<crate::keys::manager::UnusableReason>> for ffi::KeyUnusableReason {
    fn from(reason: Option<crate::keys::manager::UnusableReason>) -> Self {
        use crate::keys::manager::UnusableReason;
        match reason {
            None => ffi::KeyUnusableReason::None,
            Some(UnusableReason::NotRegistered) => ffi::KeyUnusableReason::NotRegistered,
            Some(UnusableReason::NoRoute) => ffi::KeyUnusableReason::NoRoute,
            Some(UnusableReason::DeviceNotConnected) => ffi::KeyUnusableReason::DeviceNotConnected,
            Some(UnusableReason::NoHotSecret) => ffi::KeyUnusableReason::NoHotSecret,
            Some(UnusableReason::SpOutputsPending) => ffi::KeyUnusableReason::SpOutputsPending,
            Some(UnusableReason::UnsupportedMusig) => ffi::KeyUnusableReason::UnsupportedMusig,
            Some(UnusableReason::UnsupportedOperation) => {
                ffi::KeyUnusableReason::UnsupportedOperation
            }
        }
    }
}

/// The three QR response failures the GUI words on their own; every other `Error`
/// variant (session, protocol, ...) stays `Other` and surfaces only as the free-text
/// `error` field.
impl From<&Error> for ffi::QrErrorKind {
    fn from(error: &Error) -> Self {
        match error {
            Error::RequestIdMismatch { .. } => ffi::QrErrorKind::RequestIdMismatch,
            Error::SignaturesUnsupported => ffi::QrErrorKind::SignaturesUnsupported,
            Error::MusigSession => ffi::QrErrorKind::MusigSession,
            _ => ffi::QrErrorKind::Other,
        }
    }
}
