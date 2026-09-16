//! CXX bridge for the keys module: module lifecycle, key records, routes, live
//! devices, the xpub cache and registered descriptors.
//!
//! The only file under `keys/` that names `cxx` or `crate::ffi`. Everything here
//! forwards to `KeyManager`; no policy lives in this file beyond parsing FFI inputs
//! and validating what the manager has no way to check on its own (an xpub or
//! descriptor's own cryptographic identity).

use std::str::FromStr;

use bitcoin::bip32::{DerivationPath, Fingerprint, Xpub};
use bitcoin::Network as BtcNetwork;
use bwk_sp::bwk_sign::bwk_descriptor::descriptor::Descriptor;
use bwk_sp::bwk_sign::miniscript::{DescriptorPublicKey, ForEachKey};

use crate::config::datadir;
use crate::ffi;
use crate::hwi::wallet_name;
use crate::keys::manager::{KeyManager, KeyManagerImpl, LiveDevice};
use crate::keys::record::{KeyRecord, RouteKind};
use crate::keys::store::KeyStore;
use crate::keys::KEYS_DIR_NAME;

/// Owns the manager and, from the next tasks on, the QR session, the camera and
/// the event channel. The only file in `keys/` that names `cxx` or `crate::ffi`.
pub struct KeysModule {
    inner: Option<KeyManager>,
    error: String,
}

pub fn new_keys_module(network: ffi::Network) -> Box<KeysModule> {
    Box::new(build_module(network))
}

fn build_module(network: ffi::Network) -> KeysModule {
    let network = BtcNetwork::from(network);
    let store = KeyStore::new(datadir().join(KEYS_DIR_NAME), network);
    match KeyManager::new(store, network) {
        Ok(manager) => KeysModule {
            inner: Some(manager),
            error: String::new(),
        },
        Err(e) => KeysModule {
            inner: None,
            error: format!("failed to open key store: {e}"),
        },
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
    pub fn set_network(&mut self, network: ffi::Network) -> ffi::KeyOpResult {
        let rebuilt = build_module(network);
        if rebuilt.inner.is_some() {
            *self = rebuilt;
            ok_op()
        } else {
            self.error = rebuilt.error.clone();
            failed_op(&rebuilt.error)
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
        match manager.add_route(fp, RouteKind::from(kind)) {
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
        match manager.remove_route(fp, RouteKind::from(kind)) {
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
        let order: Vec<RouteKind> = order.into_iter().map(RouteKind::from).collect();
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

impl From<ffi::KeyRouteKind> for RouteKind {
    fn from(kind: ffi::KeyRouteKind) -> Self {
        match kind {
            ffi::KeyRouteKind::Hot => RouteKind::Hot,
            ffi::KeyRouteKind::Usb => RouteKind::Usb,
            ffi::KeyRouteKind::Qr => RouteKind::Qr,
            ffi::KeyRouteKind::SdCard => RouteKind::SdCard,
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
