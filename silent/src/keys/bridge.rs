//! CXX bridge for the keys module: module lifecycle, key records, routes, live
//! devices, the xpub cache and registered descriptors.
//!
//! The only file under `keys/` that names `cxx` or `crate::ffi`. Everything here
//! forwards to `KeyManager`; no policy lives in this file beyond parsing FFI inputs
//! and validating what the manager has no way to check on its own (an xpub or
//! descriptor's own cryptographic identity).

use std::str::FromStr;

use base64::{prelude::BASE64_STANDARD, Engine};
use bitcoin::bip32::{DerivationPath, Fingerprint, Xpub};
use bitcoin::Network as BtcNetwork;
use bwk_sp::bwk_sign::bwk_descriptor::descriptor::Descriptor;
use bwk_sp::bwk_sign::miniscript::{DescriptorPublicKey, ForEachKey};
use bwk_sp::bwk_tx::bwk_psbt::Error as PsbtV2Error;
use zeroize::Zeroizing;

use crate::config::datadir;
use crate::ffi;
use crate::hwi::wallet_name;
use crate::keys::{
    crypto::{open_mnemonic, Password},
    hot::{seal_mnemonic, sign_hot, sp_descriptor},
    manager::{KeyManager, KeyManagerImpl, LiveDevice, SigningCandidate},
    psbt, qr,
    record::{KeyRecord, RouteKind, SecretEnvelope},
    store::KeyStore,
    Error, KEYS_DIR_NAME,
};

/// Owns the manager and the QR session. The only file in `keys/` that names `cxx` or
/// `crate::ffi`.
pub struct KeysModule {
    inner: Option<KeyManager>,
    error: String,
    session: qr::QrSession,
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
        for entry in &record.xpubs {
            let chains = if is_direct_child(&entry.path, &deriv_path) {
                xpub_value.parent_fingerprint == entry.xpub.fingerprint()
            } else if is_direct_child(&deriv_path, &entry.path) {
                entry.xpub.parent_fingerprint == xpub_value.fingerprint()
            } else {
                true
            };
            if !chains {
                return failed_op(&format!(
                    "xpub at '{path}' does not chain from the xpub already cached for key \
                     '{fingerprint}': wrong device"
                ));
            }
        }

        match manager.cache_xpub(fp, deriv_path, xpub_value) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("cache_xpub {fingerprint}: {e}")),
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
            })
            .collect()
    }

    pub fn register_descriptor(&mut self, fingerprint: &str, descriptor: &str) -> ffi::KeyOpResult {
        let Some(manager) = self.inner.as_mut() else {
            return failed_op(&self.error);
        };
        let fp = match parse_fingerprint(fingerprint) {
            Ok(fp) => fp,
            Err(e) => return failed_op(&e),
        };
        let parsed = match Descriptor::from_str(descriptor) {
            Ok(parsed) => parsed,
            Err(e) => return failed_op(&format!("invalid descriptor: {e}")),
        };
        // `Descriptor::fingerprint()` returns only one arbitrary key's origin (the
        // first one miniscript's traversal visits), so it cannot be used to check
        // membership in a multi-key (e.g. multisig) descriptor. For the Miniscript
        // variant, walk every key origin instead; SP descriptors are inherently
        // single-key so `fingerprint()` is still exact there.
        let has_key = match parsed.as_miniscript() {
            Some(miniscript) => {
                miniscript.for_any_key(|key| descriptor_key_fingerprint(key) == Some(fp))
            }
            None => parsed.fingerprint() == Some(fp),
        };
        if !has_key {
            return failed_op(&format!(
                "descriptor's key origins do not include key '{fingerprint}'"
            ));
        }

        let alias = wallet_name(descriptor);
        match manager.set_descriptor_registered(fp, alias, descriptor.to_string()) {
            Ok(()) => ok_op(),
            Err(e) => failed_op(&format!("register_descriptor {fingerprint}: {e}")),
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

    pub fn qr_request_get_sp_descriptor(&mut self) -> ffi::QrMessageResult {
        to_message(self.session.get_sp_descriptor())
    }

    /// Decodes a response's bytes and validates them against the outstanding request.
    pub fn qr_decode_response(&mut self, payload: &[u8]) -> Box<QrResponse> {
        Box::new(QrResponse {
            inner: self.session.decode_response(payload),
        })
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
pub fn hot_sign(material: &HotMaterial, password: &str, psbt_v2_hex: &str) -> ffi::SignResult {
    let password = Password::new(password.to_string());
    let (record, network) = match material.inner.as_ref() {
        Ok(pair) => pair,
        Err(e) => return failed_sign(e),
    };
    let psbt_bytes = match hex::decode(psbt_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_sign(&format!("invalid psbt hex: {e}")),
    };
    match sign_hot(record, *network, password, &psbt_bytes) {
        Ok(signed) => ffi::SignResult {
            is_ok: true,
            error: String::new(),
            wrong_password: false,
            psbt_v2_hex: hex::encode(signed),
        },
        Err(e) => ffi::SignResult {
            wrong_password: matches!(e, Error::WrongPassword),
            ..failed_sign(&format!("sign_hot {}: {e}", record.fingerprint))
        },
    }
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

/// Converts `psbt_v2_hex` into its v0 base64 payload for a USB signing device.
/// Cheap; safe to call from the GUI thread. Refuses a PSBT with a pending SP
/// output: a signing device only ever signs its own bip32 inputs, and the signing
/// plan already blocks every other candidate while one is pending.
pub fn usb_sign_payload(psbt_v2_hex: &str) -> ffi::PsbtPayloadResult {
    let bytes = match hex::decode(psbt_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_psbt_payload(&format!("invalid psbt hex: {e}")),
    };
    let parsed = match psbt::parse_v2(&bytes) {
        Ok(parsed) => parsed,
        Err(e) => return failed_psbt_payload(&e.to_string()),
    };
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
            psbt_v2_hex: hex::encode(merged),
        },
        Err(e) => failed_sign(&e.to_string()),
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
            Ok(qr::QrResponse::SpDescriptor { .. }) => ffi::QrResponseKind::SpDescriptor,
        }
    }

    /// The signer's fingerprint when kind() is Xpubs or SpDescriptor, else empty.
    pub fn fingerprint(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { fingerprint, .. })
            | Ok(qr::QrResponse::SpDescriptor { fingerprint, .. }) => fingerprint.to_string(),
            _ => String::new(),
        }
    }

    /// The SP descriptor when kind() is SpDescriptor, else empty.
    pub fn descriptor(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::SpDescriptor { descriptor, .. }) => descriptor.clone(),
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

    /// The derived xpubs when kind() is Xpubs, else empty.
    pub fn xpubs(&self) -> Vec<ffi::KeyXpub> {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs {
                fingerprint, xpubs, ..
            }) => xpubs
                .iter()
                .map(|(path, xpub)| ffi::KeyXpub {
                    fingerprint: fingerprint.to_string(),
                    path: path.to_string(),
                    xpub: xpub.to_string(),
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

/// True when `child` is exactly one derivation step below `parent`.
fn is_direct_child(parent: &DerivationPath, child: &DerivationPath) -> bool {
    let parent = parent.as_ref();
    let child = child.as_ref();
    child.len() == parent.len() + 1 && &child[..parent.len()] == parent
}

/// The key origin's fingerprint, if any. Mirrors the per-variant match in
/// `Descriptor::fingerprint()`, but is applied per-key via `for_any_key` instead
/// of stopping at the first key that has an origin.
fn descriptor_key_fingerprint(key: &DescriptorPublicKey) -> Option<Fingerprint> {
    match key {
        DescriptorPublicKey::Single(k) => k.origin.as_ref(),
        DescriptorPublicKey::XPub(k) => k.origin.as_ref(),
        DescriptorPublicKey::MultiXPub(k) => k.origin.as_ref(),
    }
    .map(|(fg, _)| *fg)
}

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
        }
    }
}

/// The two QR response validation failures the GUI must show translated; every
/// other `Error` variant (session, protocol, ...) stays `Other` and surfaces only as
/// the free-text `error` field.
impl From<&Error> for ffi::QrErrorKind {
    fn from(error: &Error) -> Self {
        match error {
            Error::RequestIdMismatch { .. } => ffi::QrErrorKind::RequestIdMismatch,
            Error::SignaturesUnsupported => ffi::QrErrorKind::SignaturesUnsupported,
            _ => ffi::QrErrorKind::Other,
        }
    }
}
