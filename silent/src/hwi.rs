//! CXX wrapper around bwk-hwi's device service.
//!
//! The service owns discovery: it finds devices, maintains the list, and announces a
//! change with `DevicesChanged`. This module only forwards requests and converts
//! notifications into bridge-safe values. Requests carry the caller's `ReqId` through
//! the service and back on the matching notification, so a response is matched by
//! request id and never by fingerprint (several devices can share one).

use std::str::FromStr;
use std::sync::Arc;

use bitcoin::bip32::DerivationPath;
use bitcoin::hashes::{sha256, Hash};
use bitcoin::{Network as BtcNetwork, Psbt};
use bwk_hwi::service::{
    HwiService as BwkService, SigningDevice, SigningDeviceMsg, SupportedDevice, UnsupportedReason,
};
use crossbeam::channel;

use crate::ffi::{HwiDevice, HwiDeviceState, HwiEvent, HwiEventFlag, Network};
use crate::hwi_config::PersistedBitboxNoiseConfig;

/// The service is generic over its message type; this is ours. `Shutdown` is ours
/// alone: it wakes the blocking recv on the C++ side, which dropping the sender cannot
/// do on its own because the service's listener thread holds a clone of it and keeps
/// running until it notices its own shutdown flag.
#[derive(Debug, Clone)]
pub enum HwiMessage {
    Device(Box<SigningDeviceMsg<u64>>),
    Shutdown,
}

impl From<SigningDeviceMsg<u64>> for HwiMessage {
    fn from(msg: SigningDeviceMsg<u64>) -> Self {
        HwiMessage::Device(Box::new(msg))
    }
}

type Service = BwkService<HwiMessage, u64>;

pub struct HwiService {
    inner: Arc<Service>,
    network: BtcNetwork,
    sender: Option<channel::Sender<HwiMessage>>,
    receiver: Option<channel::Receiver<HwiMessage>>,
    running: bool,
}

fn build_service(network: BtcNetwork) -> Arc<Service> {
    let inner = Service::new(network);
    inner.set_bitbox_noise_config(Arc::new(PersistedBitboxNoiseConfig));
    Arc::new(inner)
}

pub fn new_hwi_service(network: Network) -> Box<HwiService> {
    let network = BtcNetwork::from(network);
    let (sender, receiver) = channel::unbounded();
    Box::new(HwiService {
        inner: build_service(network),
        network,
        sender: Some(sender),
        receiver: Some(receiver),
        running: false,
    })
}

impl HwiService {
    /// Start polling. Idempotent: the service ref-counts starts, so this keeps at most
    /// one outstanding.
    pub fn start(&mut self) {
        if self.running {
            return;
        }
        let Some(sender) = self.sender.clone() else {
            log::error!("HwiService::start() after shutdown");
            return;
        };
        self.running = true;
        self.inner.start(sender);
    }

    pub fn stop(&mut self) {
        if !self.running {
            return;
        }
        self.running = false;
        self.inner.stop();
    }

    /// Rebuild the service on another network, keeping the notification channel. The
    /// old listener winds itself down; nothing here waits for it, so the caller's
    /// thread is never blocked on a device that is mid-transfer.
    pub fn set_network(&mut self, network: Network) {
        let network = BtcNetwork::from(network);
        if self.network == network {
            return;
        }
        let was_running = self.running;
        self.stop();
        self.network = network;
        self.inner = build_service(network);
        if was_running {
            self.start();
        }
    }

    /// Stop for good and end the notification stream.
    pub fn shutdown(&mut self) {
        self.stop();
        if let Some(sender) = self.sender.take() {
            // Wake the receiver rather than waiting for the listener to drop its clone.
            let _ = sender.send(HwiMessage::Shutdown);
        }
    }

    pub fn is_running(&self) -> bool {
        self.running
    }

    /// Handed to a dedicated C++ thread that blocks on recv. Returns an empty receiver
    /// if called twice.
    pub fn take_receiver(&mut self) -> Box<HwiReceiver> {
        Box::new(HwiReceiver {
            receiver: self.receiver.take(),
        })
    }

    /// Every device the service can see, in whatever state.
    pub fn devices(&self) -> Vec<HwiDevice> {
        self.inner
            .list()
            .into_iter()
            .map(|(id, device)| to_device(id, &device))
            .collect()
    }

    pub fn get_xpub(&self, req_id: u64, device_id: &str, path: &str) -> bool {
        let Ok(deriv) = DerivationPath::from_str(path) else {
            log::error!("HwiService::get_xpub() invalid derivation path: {path}");
            return false;
        };
        let Some(device) = self.supported(device_id) else {
            return false;
        };
        device.get_extended_pubkey(req_id, &deriv);
        true
    }

    pub fn is_descriptor_registered(&self, req_id: u64, device_id: &str, descriptor: &str) -> bool {
        let Some(device) = self.supported(device_id) else {
            return false;
        };
        device.is_wallet_registered(req_id, &wallet_name(descriptor), descriptor);
        true
    }

    pub fn register_descriptor(&self, req_id: u64, device_id: &str, descriptor: &str) -> bool {
        let Some(device) = self.supported(device_id) else {
            return false;
        };
        device.register_wallet(req_id, &wallet_name(descriptor), descriptor);
        true
    }

    pub fn sign_psbt(&self, req_id: u64, device_id: &str, psbt_base64: &str) -> bool {
        let Ok(psbt) = Psbt::from_str(psbt_base64) else {
            log::error!("HwiService::sign_psbt() invalid PSBT");
            return false;
        };
        let Some(device) = self.supported(device_id) else {
            return false;
        };
        device.sign_tx(req_id, psbt);
        true
    }

    fn supported(&self, device_id: &str) -> Option<SupportedDevice<HwiMessage, u64>> {
        match self.inner.list().remove(device_id) {
            Some(SigningDevice::Supported(device)) => Some(device),
            _ => {
                log::error!("HwiService: no usable device with id {device_id}");
                None
            }
        }
    }
}

/// Same derivation bwk-sign's `HwSigner` uses, so a descriptor registered through one
/// path is found by the other. FIXME: bwk should expose it instead of both sides
/// deriving it.
pub(crate) fn wallet_name(descriptor: &str) -> String {
    let hash = sha256::Hash::hash(descriptor.as_bytes());
    let bytes = hash.as_byte_array();
    format!(
        "{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3]
    )
}

fn to_device(id: String, device: &SigningDevice<HwiMessage, u64>) -> HwiDevice {
    let (state, version, detail) = match device {
        SigningDevice::Supported(supported) => (
            HwiDeviceState::Supported,
            supported
                .version()
                .map(ToString::to_string)
                .unwrap_or_default(),
            String::new(),
        ),
        SigningDevice::Locked { pairing_code, .. } => (
            HwiDeviceState::Locked,
            String::new(),
            pairing_code.clone().unwrap_or_default(),
        ),
        SigningDevice::Unsupported {
            version, reason, ..
        } => (
            HwiDeviceState::Unsupported,
            version
                .as_ref()
                .map(ToString::to_string)
                .unwrap_or_default(),
            unsupported_reason(reason),
        ),
    };

    HwiDevice {
        id,
        kind: device.kind().to_string(),
        fingerprint: device
            .fingerprint()
            .map(|f| f.to_string())
            .unwrap_or_default(),
        version,
        state,
        detail,
    }
}

fn unsupported_reason(reason: &UnsupportedReason) -> String {
    match reason {
        UnsupportedReason::Version {
            minimal_supported_version,
        } => format!("firmware too old, needs {minimal_supported_version} or later"),
        UnsupportedReason::Method(method) => format!("does not support {method}"),
        UnsupportedReason::NotPartOfWallet(fingerprint) => {
            format!("not part of the wallet ({fingerprint})")
        }
        UnsupportedReason::WrongNetwork => "set to a different network".to_string(),
        UnsupportedReason::AppIsNotOpen => "the Bitcoin app is not open".to_string(),
    }
}

/// A message the module has no use for yields None and the receiver waits for the next.
fn to_event(msg: SigningDeviceMsg<u64>) -> Option<HwiEvent> {
    let event = match msg {
        SigningDeviceMsg::Update => HwiEvent {
            flag: HwiEventFlag::DevicesChanged,
            ..HwiEvent::default()
        },
        SigningDeviceMsg::Error(req_id, error) => HwiEvent {
            flag: HwiEventFlag::Error,
            req_id: req_id.unwrap_or(0),
            error,
            ..HwiEvent::default()
        },
        SigningDeviceMsg::XPub(req_id, fingerprint, _, xpub) => HwiEvent {
            flag: HwiEventFlag::Xpub,
            req_id,
            fingerprint: fingerprint.to_string(),
            payload: xpub.to_string(),
            ok: true,
            ..HwiEvent::default()
        },
        SigningDeviceMsg::Version(req_id, fingerprint, version) => HwiEvent {
            flag: HwiEventFlag::Version,
            req_id,
            fingerprint: fingerprint.to_string(),
            payload: version.to_string(),
            ok: true,
            ..HwiEvent::default()
        },
        SigningDeviceMsg::WalletRegistered(req_id, fingerprint, name, _) => HwiEvent {
            flag: HwiEventFlag::WalletRegistered,
            req_id,
            fingerprint: fingerprint.to_string(),
            payload: name,
            ok: true,
            ..HwiEvent::default()
        },
        SigningDeviceMsg::WalletIsRegistered(req_id, fingerprint, name, registered) => HwiEvent {
            flag: HwiEventFlag::WalletIsRegistered,
            req_id,
            fingerprint: fingerprint.to_string(),
            payload: name,
            ok: registered,
            ..HwiEvent::default()
        },
        SigningDeviceMsg::TransactionSigned(req_id, fingerprint, psbt) => HwiEvent {
            flag: HwiEventFlag::TransactionSigned,
            req_id,
            fingerprint: fingerprint.to_string(),
            payload: psbt.to_string(),
            ok: true,
            ..HwiEvent::default()
        },
        // Never requested: nothing calls display_address.
        SigningDeviceMsg::AddressDisplayed(..) => return None,
    };
    Some(event)
}

impl Default for HwiEvent {
    fn default() -> Self {
        HwiEvent {
            flag: HwiEventFlag::DevicesChanged,
            req_id: 0,
            fingerprint: String::new(),
            payload: String::new(),
            error: String::new(),
            ok: false,
        }
    }
}

pub struct HwiReceiver {
    receiver: Option<channel::Receiver<HwiMessage>>,
}

impl HwiReceiver {
    /// Blocks until the next usable notification. Returns an empty poll once the
    /// service is shut down.
    pub fn recv(&self) -> Box<HwiPoll> {
        let Some(receiver) = self.receiver.as_ref() else {
            return Box::new(HwiPoll::none());
        };
        loop {
            match receiver.recv() {
                Ok(HwiMessage::Device(msg)) => {
                    if let Some(event) = to_event(*msg) {
                        return Box::new(HwiPoll::ok(event));
                    }
                }
                Ok(HwiMessage::Shutdown) | Err(_) => return Box::new(HwiPoll::none()),
            }
        }
    }
}

pub struct HwiPoll {
    is_some: bool,
    event: HwiEvent,
}

impl HwiPoll {
    fn ok(event: HwiEvent) -> Self {
        HwiPoll {
            is_some: true,
            event,
        }
    }

    fn none() -> Self {
        HwiPoll {
            is_some: false,
            event: HwiEvent::default(),
        }
    }

    pub fn is_some(&self) -> bool {
        self.is_some
    }

    pub fn get_event(&self) -> HwiEvent {
        self.event.clone()
    }
}
