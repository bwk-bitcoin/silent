//! CXX bridge for the keys module: module lifecycle, key records, routes, live
//! devices, the xpub cache and registered descriptors.
//!
//! The only file under `keys/` that names `cxx` or `crate::ffi`. Everything here
//! forwards to `KeyManager`; no policy lives in this file beyond parsing FFI inputs
//! and validating what the manager has no way to check on its own (an xpub or
//! descriptor's own cryptographic identity).

use std::{
    str::FromStr,
    sync::{Arc, Mutex},
};

use base64::{prelude::BASE64_STANDARD, Engine};
use bitcoin::bip32::{DerivationPath, Fingerprint, Xpub};
use bitcoin::Network as BtcNetwork;
use bwk_sp::bwk_sign::bwk_descriptor::descriptor::Descriptor;
use bwk_sp::bwk_sign::miniscript::{DescriptorPublicKey, ForEachKey};
use bwk_sp::bwk_tx::bwk_psbt::Error as PsbtV2Error;
use crossbeam::channel;
use zeroize::Zeroizing;

use crate::config::datadir;
use crate::ffi;
use crate::hwi::wallet_name;
use crate::keys::{
    camera::{list_cameras, Camera, CameraMessage},
    crypto::{open_mnemonic, Password},
    hot::{seal_mnemonic, sign_hot, sp_descriptor},
    manager::{KeyManager, KeyManagerImpl, LiveDevice, SigningCandidate},
    psbt, qr,
    record::{KeyRecord, RouteKind, SecretEnvelope},
    store::KeyStore,
    Error, KEYS_DIR_NAME,
};

/// Owns the manager, the QR session, the camera and the event channel. The only
/// file in `keys/` that names `cxx` or `crate::ffi`.
pub struct KeysModule {
    inner: Option<KeyManager>,
    error: String,
    /// Shared with every `KeysReceiver` taken from this module, so a response
    /// scanned on the receiver's thread validates against the same outstanding
    /// request as one pasted on the GUI thread.
    session: Option<Arc<Mutex<qr::QrSession>>>,
    camera: Option<Camera>,
    sender: Option<channel::Sender<KeysMessage>>,
    receiver: Option<channel::Receiver<KeysMessage>>,
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

/// Opens the manager and QR session for `network`. Both must succeed or the whole
/// module is unusable: nothing here works with only one of the two, so tracking
/// two independent failure states would only add confusion.
fn open(network: ffi::Network) -> Result<(KeyManager, Arc<Mutex<qr::QrSession>>), String> {
    let manager = open_manager(network)?;
    let session = qr::QrSession::new().map_err(|e| format!("failed to init QR session: {e}"))?;
    Ok((manager, Arc::new(Mutex::new(session))))
}

fn build_module(network: ffi::Network) -> KeysModule {
    let (sender, receiver) = channel::unbounded();
    let (inner, session, error) = match open(network) {
        Ok((manager, session)) => (Some(manager), Some(session), String::new()),
        Err(error) => (None, None, error),
    };
    KeysModule {
        inner,
        error,
        session,
        camera: None,
        sender: Some(sender),
        receiver: Some(receiver),
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
    /// Keeps the event channel and any running camera, same as
    /// `HwiService::set_network`.
    ///
    /// The QR session is reset in place rather than replaced: a `KeysReceiver`
    /// taken before the switch holds its own clone of the same `Arc`, so
    /// swapping in a fresh `Arc` here would leave that receiver validating
    /// scanned responses against a stale session nobody else can reach.
    pub fn set_network(&mut self, network: ffi::Network) -> ffi::KeyOpResult {
        match open_manager(network) {
            Ok(manager) => {
                let reset = match self.session.as_ref() {
                    Some(session) => match session.lock() {
                        Ok(mut session) => session.reset().map_err(|e| e.to_string()),
                        Err(_) => Err("QR session lock poisoned".to_string()),
                    },
                    None => match qr::QrSession::new() {
                        Ok(session) => {
                            self.session = Some(Arc::new(Mutex::new(session)));
                            Ok(())
                        }
                        Err(e) => Err(e.to_string()),
                    },
                };
                match reset {
                    Ok(()) => {
                        self.inner = Some(manager);
                        self.error = String::new();
                        ok_op()
                    }
                    Err(error) => {
                        let error = format!("failed to reset QR session: {error}");
                        self.error = error.clone();
                        failed_op(&error)
                    }
                }
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

    /// Stop the camera for good and end the notification stream.
    pub fn shutdown(&mut self) {
        self.camera_stop();
        if let Some(sender) = self.sender.take() {
            // Wake the receiver rather than waiting for it to notice a disconnect.
            let _ = sender.send(KeysMessage::Shutdown);
        }
    }

    /// Handed to a dedicated C++ thread that blocks on recv. Returns an empty
    /// receiver if called twice.
    pub fn take_receiver(&mut self) -> Box<KeysReceiver> {
        Box::new(KeysReceiver {
            receiver: self.receiver.take(),
            session: self.session.clone(),
        })
    }

    pub fn qr_request_get_xpubs(&mut self, paths: Vec<String>, part_bytes: u32) -> Box<QrRequest> {
        let mut parsed = Vec::with_capacity(paths.len());
        for path in &paths {
            match DerivationPath::from_str(path) {
                Ok(path) => parsed.push(path),
                Err(e) => {
                    return Box::new(QrRequest {
                        inner: Err(format!("invalid derivation path '{path}': {e}")),
                    })
                }
            }
        }
        let inner = self.with_session(|session| session.get_xpubs(parsed, part_bytes as usize));
        Box::new(QrRequest { inner })
    }

    pub fn qr_request_register_descriptor(
        &mut self,
        descriptor: &str,
        part_bytes: u32,
    ) -> Box<QrRequest> {
        let inner = self
            .with_session(|session| session.register_descriptor(descriptor, part_bytes as usize));
        Box::new(QrRequest { inner })
    }

    pub fn qr_request_verify_address(
        &mut self,
        descriptor: &str,
        path: &str,
        address: &str,
        part_bytes: u32,
    ) -> Box<QrRequest> {
        let path = match DerivationPath::from_str(path) {
            Ok(path) => path,
            Err(e) => {
                return Box::new(QrRequest {
                    inner: Err(format!("invalid derivation path '{path}': {e}")),
                })
            }
        };
        let inner = self.with_session(|session| {
            session.verify_address(descriptor, path, address.to_string(), part_bytes as usize)
        });
        Box::new(QrRequest { inner })
    }

    pub fn qr_request_sign(
        &mut self,
        descriptors: Vec<String>,
        psbt_v2_hex: &str,
        part_bytes: u32,
    ) -> Box<QrRequest> {
        let psbt_bytes = match hex::decode(psbt_v2_hex) {
            Ok(bytes) => bytes,
            Err(e) => {
                return Box::new(QrRequest {
                    inner: Err(format!("invalid psbt hex: {e}")),
                })
            }
        };
        let inner = self
            .with_session(|session| session.sign(&descriptors, &psbt_bytes, part_bytes as usize));
        Box::new(QrRequest { inner })
    }

    pub fn qr_request_get_sp_descriptor(&mut self, part_bytes: u32) -> Box<QrRequest> {
        let inner = self.with_session(|session| session.get_sp_descriptor(part_bytes as usize));
        Box::new(QrRequest { inner })
    }

    /// Validates a pasted payload against the outstanding request id.
    pub fn qr_decode_response(&mut self, payload: &str) -> Box<QrResponse> {
        let bytes = match BASE64_STANDARD.decode(payload.trim()) {
            Ok(bytes) => bytes,
            Err(e) => {
                return Box::new(QrResponse {
                    inner: Err(format!("invalid base64: {e}")),
                })
            }
        };
        let inner = self.with_session(|session| session.decode_response(&bytes));
        Box::new(QrResponse { inner })
    }

    /// Locks the session for one call, converting any `keys::Error` into the
    /// bridge's flat string error. Errors with `self.error` when the module has
    /// no session (it failed to build) and with a dedicated message if the lock
    /// is poisoned.
    fn with_session<T>(
        &self,
        f: impl FnOnce(&mut qr::QrSession) -> Result<T, Error>,
    ) -> Result<T, String> {
        let Some(session) = self.session.as_ref() else {
            return Err(self.error.clone());
        };
        let Ok(mut session) = session.lock() else {
            return Err("QR session lock poisoned".to_string());
        };
        f(&mut session).map_err(|e| e.to_string())
    }

    pub fn camera_list(&self) -> ffi::CameraListResult {
        match list_cameras() {
            Ok(infos) => ffi::CameraListResult {
                is_ok: true,
                error: String::new(),
                names: infos.iter().map(|info| info.name.clone()).collect(),
                indexes: infos.iter().map(|info| info.index).collect(),
            },
            Err(e) => ffi::CameraListResult {
                is_ok: false,
                error: e.to_string(),
                names: Vec::new(),
                indexes: Vec::new(),
            },
        }
    }

    /// Stops the current camera, if any, and stores the one `camera_open` built,
    /// forwarding its messages over this module's own event channel.
    // `handle` must stay boxed: it crosses the cxx bridge as an owned opaque type,
    // which only `Box<T>` can represent.
    #[allow(clippy::boxed_local)]
    pub fn camera_install(&mut self, handle: Box<CameraHandle>) -> ffi::KeyOpResult {
        self.camera_stop();
        let (camera, camera_receiver) = match handle.inner {
            Ok(pair) => pair,
            Err(e) => return failed_op(&e),
        };
        let Some(sender) = self.sender.clone() else {
            return failed_op("keys module event channel is not available");
        };
        if let Err(e) = forward_camera_messages(camera_receiver, sender) {
            return failed_op(&format!(
                "camera_install: failed to spawn forwarding thread: {e}"
            ));
        }
        self.camera = Some(camera);
        ok_op()
    }

    /// Idempotent: dropping `None` is a no-op, and `Camera::drop` stops capture.
    pub fn camera_stop(&mut self) {
        self.camera = None;
    }

    pub fn camera_frame(&self) -> ffi::CameraFrameResult {
        let Some(camera) = self.camera.as_ref() else {
            return failed_camera_frame("no camera running");
        };
        match camera.preview() {
            Some(preview) => ffi::CameraFrameResult {
                is_ok: true,
                error: String::new(),
                data: preview.data,
                width: preview.width,
                height: preview.height,
                seq: preview.seq,
            },
            None => failed_camera_frame("no frame captured yet"),
        }
    }

    pub fn camera_progress(&self) -> ffi::ScanProgress {
        let Some(camera) = self.camera.as_ref() else {
            return failed_scan_progress("no camera running");
        };
        let progress = camera.progress();
        ffi::ScanProgress {
            is_ok: true,
            error: String::new(),
            seen: progress.seen as u32,
            total: progress.total as u32,
            seq: progress.seq,
        }
    }

    /// Drops the partial BBQR reassembly, for a rescan.
    pub fn camera_reset(&mut self) {
        if let Some(camera) = self.camera.as_ref() {
            camera.reset();
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
        },
        Err(e) => failed_mnemonic(&format!("reveal {}: {e}", record.fingerprint)),
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
            psbt_v2_hex: hex::encode(signed),
        },
        Err(e) => failed_sign(&format!("sign_hot {}: {e}", record.fingerprint)),
    }
}

/// The BIP392 sp() descriptor of `material`'s hot key, origin included, for account
/// creation. Blocks on Argon2id while opening the envelope: call from a QThread,
/// never from the GUI thread.
pub fn hot_sp_descriptor(material: &HotMaterial, password: &str) -> ffi::SpDescriptorResult {
    let password = Password::new(password.to_string());
    let (record, network) = match material.inner.as_ref() {
        Ok(pair) => pair,
        Err(e) => return failed_sp_descriptor(e),
    };
    match sp_descriptor(record, *network, password) {
        Ok(descriptor) => ffi::SpDescriptorResult {
            is_ok: true,
            error: String::new(),
            descriptor: descriptor.to_string(),
            fingerprint: record.fingerprint.to_string(),
        },
        Err(e) => failed_sp_descriptor(&format!("hot_sp_descriptor {}: {e}", record.fingerprint)),
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
            psbt_v2_hex: hex::encode(merged),
        },
        Err(e) => failed_sign(&e.to_string()),
    }
}

/// A camera opened by the free `camera_open`, or the error that refused it. Installed
/// into a `KeysModule` by `camera_install` on the GUI thread.
pub struct CameraHandle {
    inner: Result<(Camera, channel::Receiver<CameraMessage>), String>,
}

impl CameraHandle {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }
}

/// Opens camera `index` on a fresh channel, touching no `KeysModule` state. Only
/// blocks briefly to spawn the capture thread: the device itself opens
/// asynchronously inside `capture_loop`, with success or failure reported later
/// over the channel as a `CameraMessage`. Still call from a QThread, never from
/// the GUI thread, for consistency with the other worker functions.
pub fn camera_open(index: u32) -> Box<CameraHandle> {
    let (camera_sender, camera_receiver) = channel::unbounded();
    let inner = Camera::start(index, camera_sender)
        .map(|camera| (camera, camera_receiver))
        .map_err(|e| format!("camera_open {index}: {e}"));
    Box::new(CameraHandle { inner })
}

/// Wraps a built (or failed) QR request. A builder never returns a null box and
/// never returns a request with no frames: `qr::QrSession`'s builders refuse
/// those cases themselves.
pub struct QrRequest {
    inner: Result<qr::QrRequest, String>,
}

impl QrRequest {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }

    pub fn req_id(&self) -> u64 {
        match self.inner.as_ref() {
            Ok(request) => request_id_to_u64(request.id()),
            Err(_) => 0,
        }
    }

    pub fn frame_count(&self) -> u32 {
        match self.inner.as_ref() {
            Ok(request) => request.frame_count() as u32,
            Err(_) => 0,
        }
    }

    /// Frame `index` of the animated request. An index past the end answers with
    /// is_ok false, never with frame 0.
    pub fn frame(&self, index: u32) -> ffi::QrImageResult {
        let Ok(request) = self.inner.as_ref() else {
            return failed_image(&self.get_error());
        };
        match request.frame(index as usize) {
            Ok(image) => ffi::QrImageResult {
                is_ok: true,
                error: String::new(),
                data: image.data.clone(),
                width: image.width,
                height: image.height,
            },
            Err(e) => failed_image(&e.to_string()),
        }
    }

    /// The whole encoded request, base64, for the paste box.
    pub fn base64(&self) -> String {
        match self.inner.as_ref() {
            Ok(request) => BASE64_STANDARD.encode(request.message()),
            Err(_) => String::new(),
        }
    }
}

/// Wraps a decoded and validated QR response, or the error that rejected it. An
/// accessor for a kind the response is not returns an empty value.
pub struct QrResponse {
    inner: Result<qr::QrResponse, String>,
}

impl QrResponse {
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    pub fn get_error(&self) -> String {
        self.inner.as_ref().err().cloned().unwrap_or_default()
    }

    /// Only meaningful when is_ok(). `Xpubs` is an arbitrary sentinel on error.
    pub fn kind(&self) -> ffi::QrResponseKind {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Xpubs { .. }) | Err(_) => ffi::QrResponseKind::Xpubs,
            Ok(qr::QrResponse::Registration { .. }) => ffi::QrResponseKind::Registration,
            Ok(qr::QrResponse::AddressUri(_)) => ffi::QrResponseKind::AddressUri,
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

    /// The signed PSBT as v2 hex when kind() is SignedPsbt, else empty.
    pub fn psbt_hex(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::SignedPsbt(bytes)) => hex::encode(bytes),
            _ => String::new(),
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

    /// The address URI when kind() is AddressUri, else empty.
    pub fn uri(&self) -> String {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::AddressUri(uri)) => uri.clone(),
            _ => String::new(),
        }
    }

    pub fn registered(&self) -> bool {
        match self.inner.as_ref() {
            Ok(qr::QrResponse::Registration { registered, .. }) => *registered,
            _ => false,
        }
    }
}

/// The service is generic over its message type; this is ours. `Shutdown` is ours
/// alone: it wakes the blocking recv on the C++ side, which dropping the sender
/// cannot do while the capture thread's forwarder holds a clone.
enum KeysMessage {
    Camera(CameraMessage),
    Shutdown,
}

/// `Camera::start` needs a concrete `Sender<CameraMessage>`, not the module's
/// `Sender<KeysMessage>`, so capture events are relayed on a small forwarding
/// thread instead of being sent directly. The thread ends on its own once the
/// capture thread's `Stopped` message drops the last `CameraMessage` sender.
fn forward_camera_messages(
    camera_receiver: channel::Receiver<CameraMessage>,
    sender: channel::Sender<KeysMessage>,
) -> Result<(), String> {
    std::thread::Builder::new()
        .name("keys-camera-forward".to_string())
        .spawn(move || {
            while let Ok(msg) = camera_receiver.recv() {
                if sender.send(KeysMessage::Camera(msg)).is_err() {
                    return;
                }
            }
        })
        .map(|_| ())
        .map_err(|e| e.to_string())
}

pub struct KeysReceiver {
    receiver: Option<channel::Receiver<KeysMessage>>,
    session: Option<Arc<Mutex<qr::QrSession>>>,
}

impl KeysReceiver {
    /// Blocks until the next usable notification. Returns an empty poll once the
    /// module is shut down.
    pub fn recv(&self) -> Box<KeysPoll> {
        let Some(receiver) = self.receiver.as_ref() else {
            return Box::new(KeysPoll::none());
        };
        loop {
            match receiver.recv() {
                Ok(KeysMessage::Camera(msg)) => {
                    if let Some(event) = to_event(msg, self.session.as_ref()) {
                        return Box::new(KeysPoll::ok(event));
                    }
                }
                Ok(KeysMessage::Shutdown) | Err(_) => return Box::new(KeysPoll::none()),
            }
        }
    }
}

pub struct KeysPoll {
    is_some: bool,
    event: ffi::KeyEvent,
}

impl KeysPoll {
    fn ok(event: ffi::KeyEvent) -> Self {
        KeysPoll {
            is_some: true,
            event,
        }
    }

    fn none() -> Self {
        KeysPoll {
            is_some: false,
            event: ffi::KeyEvent::default(),
        }
    }

    pub fn is_some(&self) -> bool {
        self.is_some
    }

    pub fn get_event(&self) -> ffi::KeyEvent {
        self.event.clone()
    }
}

impl Default for ffi::KeyEvent {
    fn default() -> Self {
        ffi::KeyEvent {
            flag: ffi::KeyEventFlag::QrDecoded,
            req_id: 0,
            fingerprint: String::new(),
            payload: String::new(),
            error: String::new(),
            ok: false,
        }
    }
}

/// A message the module has no use for yields None and the receiver waits for the
/// next. Camera events carry no `req_id` to answer: a scan is unsolicited from the
/// event channel's point of view, even when it happens to satisfy an outstanding
/// QR request.
fn to_event(
    msg: CameraMessage,
    session: Option<&Arc<Mutex<qr::QrSession>>>,
) -> Option<ffi::KeyEvent> {
    match msg {
        CameraMessage::Decoded(decoded) => decoded_to_event(decoded, session),
        CameraMessage::Progress => Some(ffi::KeyEvent {
            flag: ffi::KeyEventFlag::ScanProgress,
            ..ffi::KeyEvent::default()
        }),
        CameraMessage::Failed(error) => Some(ffi::KeyEvent {
            flag: ffi::KeyEventFlag::Error,
            error,
            ..ffi::KeyEvent::default()
        }),
        CameraMessage::Stopped => Some(ffi::KeyEvent {
            flag: ffi::KeyEventFlag::CameraStopped,
            ..ffi::KeyEvent::default()
        }),
    }
}

/// Runs a camera-decoded response through the same `validate_response` a pasted
/// payload uses, so both get identical checks.
fn decoded_to_event(
    decoded: bwk_qr::decoder::Decoded,
    session: Option<&Arc<Mutex<qr::QrSession>>>,
) -> Option<ffi::KeyEvent> {
    let response = match decoded {
        bwk_qr::decoder::Decoded::Response(response) => response,
        // Requests and plain text carry nothing this module has a use for.
        bwk_qr::decoder::Decoded::Text(_) | bwk_qr::decoder::Decoded::Request(_) => return None,
    };
    let Some(session) = session else {
        return Some(ffi::KeyEvent {
            flag: ffi::KeyEventFlag::Error,
            error: "QR session is not available".to_string(),
            ..ffi::KeyEvent::default()
        });
    };
    let Ok(mut session) = session.lock() else {
        return Some(ffi::KeyEvent {
            flag: ffi::KeyEventFlag::Error,
            error: "QR session lock poisoned".to_string(),
            ..ffi::KeyEvent::default()
        });
    };
    Some(match session.validate_response(response) {
        Ok(parsed) => qr_response_to_event(parsed),
        Err(e) => ffi::KeyEvent {
            flag: ffi::KeyEventFlag::Error,
            error: e.to_string(),
            ..ffi::KeyEvent::default()
        },
    })
}

/// Flattens a decoded response into the event's generic fields. `Xpubs` has no
/// field for a list, so its entries are joined as `path=xpub` pairs.
fn qr_response_to_event(response: qr::QrResponse) -> ffi::KeyEvent {
    match response {
        qr::QrResponse::SpDescriptor {
            descriptor,
            fingerprint,
        } => ffi::KeyEvent {
            flag: ffi::KeyEventFlag::QrDecoded,
            fingerprint: fingerprint.to_string(),
            payload: descriptor,
            ok: true,
            ..ffi::KeyEvent::default()
        },
        qr::QrResponse::Registration { alias, registered } => ffi::KeyEvent {
            flag: ffi::KeyEventFlag::QrDecoded,
            payload: alias,
            ok: registered,
            ..ffi::KeyEvent::default()
        },
        qr::QrResponse::AddressUri(uri) => ffi::KeyEvent {
            flag: ffi::KeyEventFlag::QrDecoded,
            payload: uri,
            ok: true,
            ..ffi::KeyEvent::default()
        },
        qr::QrResponse::SignedPsbt(bytes) => ffi::KeyEvent {
            flag: ffi::KeyEventFlag::QrDecoded,
            payload: hex::encode(bytes),
            ok: true,
            ..ffi::KeyEvent::default()
        },
        qr::QrResponse::Xpubs {
            fingerprint, xpubs, ..
        } => ffi::KeyEvent {
            flag: ffi::KeyEventFlag::QrDecoded,
            fingerprint: fingerprint.to_string(),
            payload: xpubs
                .iter()
                .map(|(path, xpub)| format!("{path}={xpub}"))
                .collect::<Vec<_>>()
                .join(";"),
            ok: true,
            ..ffi::KeyEvent::default()
        },
    }
}

fn request_id_to_u64(id: bwk_qr_protocol::RequestId) -> u64 {
    u64::from(u16::from_be_bytes(id.0))
}

fn failed_image(error: &str) -> ffi::QrImageResult {
    ffi::QrImageResult {
        is_ok: false,
        error: error.to_string(),
        data: Vec::new(),
        width: 0,
        height: 0,
    }
}

fn failed_camera_frame(error: &str) -> ffi::CameraFrameResult {
    ffi::CameraFrameResult {
        is_ok: false,
        error: error.to_string(),
        data: Vec::new(),
        width: 0,
        height: 0,
        seq: 0,
    }
}

fn failed_scan_progress(error: &str) -> ffi::ScanProgress {
    ffi::ScanProgress {
        is_ok: false,
        error: error.to_string(),
        seen: 0,
        total: 0,
        seq: 0,
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
    }
}

fn failed_sign(error: &str) -> ffi::SignResult {
    ffi::SignResult {
        is_ok: false,
        error: error.to_string(),
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

fn failed_sp_descriptor(error: &str) -> ffi::SpDescriptorResult {
    ffi::SpDescriptorResult {
        is_ok: false,
        error: error.to_string(),
        descriptor: String::new(),
        fingerprint: String::new(),
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
