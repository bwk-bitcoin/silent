//! CXX wrapper around bwk-hwi's device service.
//!
//! The service owns discovery: it finds devices, maintains the list, and announces a
//! change with `DevicesChanged`. This module only forwards requests and converts
//! notifications into bridge-safe values. Requests carry the caller's `ReqId` through
//! the service and back on the matching notification, so a response is matched by
//! request id and never by fingerprint (several devices can share one).

use std::str::FromStr;
use std::sync::Arc;

use bitcoin::bip32::{ChildNumber, DerivationPath};
use bitcoin::hashes::{sha256, Hash};
use bitcoin::{Network as BtcNetwork, Psbt};
use bwk_hwi::{
    service::{
        HwiService as BwkService, SigningDevice, SigningDeviceMsg, SupportedDevice,
        UnsupportedReason,
    },
    AddressScript, DeviceWallet, MUSIG_SESSION_ERROR,
};
use crossbeam::channel;

use crate::ffi::{HwiDevice, HwiDeviceState, HwiErrorKind, HwiEvent, HwiEventFlag, Network};
use crate::hwi_config::PersistedBitboxNoiseConfig;
use crate::keys::hwi_capabilities;

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
        // No display: a device shows the path on its own when it asks the user.
        device.get_extended_pubkey(req_id, &deriv, false);
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

    pub fn sign_psbt(
        &self,
        req_id: u64,
        device_id: &str,
        psbt_base64: &str,
        descriptor: &str,
        hmac: &str,
    ) -> bool {
        let Ok(psbt) = Psbt::from_str(psbt_base64) else {
            log::error!("HwiService::sign_psbt() invalid PSBT");
            return false;
        };
        let Some(wallet) = device_wallet("sign_psbt", descriptor, hmac) else {
            return false;
        };
        let Some(device) = self.supported(device_id) else {
            return false;
        };
        device.sign_tx_with_wallet(req_id, psbt, wallet);
        true
    }

    pub fn display_address(
        &self,
        req_id: u64,
        device_id: &str,
        descriptor: &str,
        path: &str,
        hmac: &str,
    ) -> bool {
        let script = match address_script(descriptor, path) {
            Ok(script) => script,
            Err(e) => {
                log::error!("HwiService::display_address() {e}");
                return false;
            }
        };
        let Some(wallet) = device_wallet("display_address", descriptor, hmac) else {
            return false;
        };
        let Some(device) = self.supported(device_id) else {
            return false;
        };
        device.display_address_with_wallet(req_id, &script, wallet);
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

/// The registered wallet of `descriptor`, as `register_descriptor` named it, with the
/// stored proof `hmac` (hex, empty when none). None, logged, on an invalid `hmac`.
fn device_wallet(method: &str, descriptor: &str, hmac: &str) -> Option<DeviceWallet> {
    match parse_hmac(hmac) {
        Ok(hmac) => Some(DeviceWallet {
            name: wallet_name(descriptor),
            policy: descriptor.to_string(),
            hmac,
        }),
        Err(e) => {
            log::error!("HwiService::{method}() invalid hmac: {e}");
            None
        }
    }
}

/// A stored registration proof: empty is none, else 64 hex characters.
pub(crate) fn parse_hmac(hmac: &str) -> Result<Option<[u8; 32]>, String> {
    if hmac.is_empty() {
        return Ok(None);
    }
    let mut bytes = [0u8; 32];
    hex::decode_to_slice(hmac, &mut bytes)
        .map_err(|e| format!("expected 64 hex characters, got {}: {e}", hmac.len()))?;
    Ok(Some(bytes))
}

/// What the device shows for `path` of `descriptor`: an absolute BIP-86 path (one
/// with a hardened step) of a single key taproot account directly, every other
/// address as the registered wallet's address at the last two steps `change/index`
/// (the musig creator and account give `0/1`, `1/5`).
pub(crate) fn address_script(descriptor: &str, path: &str) -> Result<AddressScript, String> {
    let deriv = DerivationPath::from_str(path)
        .map_err(|e| format!("invalid derivation path {path}: {e}"))?;
    let steps = deriv.as_ref();
    if (descriptor.is_empty() || descriptor.starts_with("tr("))
        && steps.iter().any(ChildNumber::is_hardened)
    {
        return Ok(AddressScript::P2TR(deriv));
    }
    match steps {
        [.., ChildNumber::Normal { index: change }, ChildNumber::Normal { index }] => {
            Ok(AddressScript::Miniscript {
                index: *index,
                change: *change != 0,
            })
        }
        _ => Err(format!("path {path} does not end with change/index")),
    }
}

/// The kind of a device error `message`, as the service forwards it in text.
fn error_kind(message: &str) -> HwiErrorKind {
    if message == MUSIG_SESSION_ERROR {
        HwiErrorKind::MusigSession
    } else {
        HwiErrorKind::Other
    }
}

/// Same derivation bwk-sign's `HwSigner` uses, so a descriptor registered through one
/// path is found by the other. The Ledger HMAC covers this name, so its derivation must
/// never change: every stored HMAC would stop matching. FIXME: bwk should expose it
/// instead of both sides deriving it.
pub(crate) fn wallet_name(descriptor: &str) -> String {
    let hash = sha256::Hash::hash(descriptor.as_bytes());
    let bytes = hash.as_byte_array();
    format!(
        "{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3]
    )
}

fn to_device(id: String, device: &SigningDevice<HwiMessage, u64>) -> HwiDevice {
    let (state, version, capabilities, detail) = match device {
        SigningDevice::Supported(supported) => (
            HwiDeviceState::Supported,
            supported
                .version()
                .map(ToString::to_string)
                .unwrap_or_default(),
            hwi_capabilities(supported.capabilities()),
            String::new(),
        ),
        SigningDevice::Locked { pairing_code, .. } => (
            HwiDeviceState::Locked,
            String::new(),
            0,
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
            0,
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
        capabilities,
        state,
        detail,
    }
}

/// Short, log-only rendering of what was shown on the device screen.
fn render_address_script(script: &AddressScript) -> String {
    match script {
        AddressScript::P2TR(path) => path.to_string(),
        AddressScript::Miniscript { index, change } => format!("{change}/{index}"),
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
            error_kind: error_kind(&error),
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
        SigningDeviceMsg::WalletRegistered(req_id, fingerprint, name, hmac) => HwiEvent {
            flag: HwiEventFlag::WalletRegistered,
            req_id,
            fingerprint: fingerprint.to_string(),
            payload: name,
            ok: true,
            hmac: hmac.map(hex::encode).unwrap_or_default(),
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
        SigningDeviceMsg::AddressDisplayed(req_id, fingerprint, script) => HwiEvent {
            flag: HwiEventFlag::AddressDisplayed,
            req_id,
            fingerprint: fingerprint.to_string(),
            payload: render_address_script(&script),
            ok: true,
            ..HwiEvent::default()
        },
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
            error_kind: HwiErrorKind::Other,
            hmac: String::new(),
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

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use bitcoin::bip32::{DerivationPath, Fingerprint};
    use bwk_hwi::{service::SigningDeviceMsg, AddressScript, Error, MUSIG_SESSION_ERROR};

    use crate::{
        ffi::{HwiErrorKind, HwiEventFlag},
        hwi::{address_script, error_kind, parse_hmac, to_event, wallet_name},
    };

    /// Shape of a musig creator descriptor; `address_script` only reads its prefix.
    const MUSIG_DESCRIPTOR: &str =
        "tr(musig([73c5da0a/87'/1'/0']tpubA/<0;1>/*,[a5a5a5a5/87'/1'/0']tpubB/<0;1>/*))";

    // ===== HMAC =====

    #[test]
    fn parse_hmac_accepts_empty_and_32_bytes() {
        assert_eq!(parse_hmac(""), Ok(None), "an empty hmac is none");
        let hmac = parse_hmac(&"ab".repeat(32));
        assert_eq!(
            hmac,
            Ok(Some([0xab; 32])),
            "64 hex characters, got {hmac:?}"
        );
    }

    #[test]
    fn parse_hmac_refuses_bad_hex_and_length() {
        for bad in ["a".repeat(63), "a".repeat(65), "zz".repeat(32)] {
            let parsed = parse_hmac(&bad);
            assert!(parsed.is_err(), "expected {bad:?} refused, got {parsed:?}");
        }
    }

    // ===== Address script =====

    #[test]
    fn address_script_of_a_musig_descriptor_is_miniscript() {
        let receive = address_script(MUSIG_DESCRIPTOR, "0/1");
        assert_eq!(
            receive,
            Ok(AddressScript::Miniscript {
                index: 1,
                change: false
            }),
            "receive address 1, got {receive:?}"
        );
        let change = address_script(MUSIG_DESCRIPTOR, "1/5");
        assert_eq!(
            change,
            Ok(AddressScript::Miniscript {
                index: 5,
                change: true
            }),
            "change address 5, got {change:?}"
        );
    }

    #[test]
    fn address_script_of_a_bip86_path_is_p2tr() {
        let script = address_script("tr([73c5da0a/86'/1'/0']tpubA/<0;1>/*)", "m/86'/1'/0'/0/3");
        assert_eq!(
            script,
            Ok(AddressScript::P2TR(
                DerivationPath::from_str("m/86'/1'/0'/0/3").unwrap()
            )),
            "a BIP-86 path is shown directly, got {script:?}"
        );
    }

    #[test]
    fn address_script_refuses_a_path_without_change_and_index() {
        for path in ["5", "m/48'/1'/0'/2'", "x/y"] {
            let script = address_script("wsh(sortedmulti(2,a,b))", path);
            assert!(script.is_err(), "expected {path} refused, got {script:?}");
        }
    }

    // ===== Error kind =====

    #[test]
    fn musig_session_error_has_its_kind() {
        for message in [
            MUSIG_SESSION_ERROR.to_string(),
            Error::MusigSession.to_string(),
        ] {
            let kind = error_kind(&message);
            assert!(
                matches!(kind, HwiErrorKind::MusigSession),
                "expected MusigSession for {message:?}, got {kind:?}"
            );
        }
        let message = Error::UserRefused.to_string();
        let kind = error_kind(&message);
        assert!(
            matches!(kind, HwiErrorKind::Other),
            "expected Other for {message:?}, got {kind:?}"
        );
    }

    #[test]
    fn musig_session_error_event_carries_its_kind() {
        let event = to_event(SigningDeviceMsg::Error(Some(7), MUSIG_SESSION_ERROR.into()))
            .expect("an error event");
        assert!(
            matches!(event.flag, HwiEventFlag::Error),
            "expected an Error event, got {:?}",
            event.flag
        );
        assert!(
            matches!(event.error_kind, HwiErrorKind::MusigSession),
            "expected MusigSession, got {:?}",
            event.error_kind
        );
        assert_eq!(event.req_id, 7, "request id of the event");
    }

    // ===== Wallet name =====

    #[test]
    fn wallet_name_is_stable() {
        // the Ledger HMACs already stored depend on this exact value
        let name = wallet_name(MUSIG_DESCRIPTOR);
        assert_eq!(name, "5e7aad2c", "wallet name of {MUSIG_DESCRIPTOR}");
    }

    #[test]
    fn wallet_registered_carries_the_hmac_hex() {
        let fingerprint = Fingerprint::from([0x73, 0xc5, 0xda, 0x0a]);
        let event = to_event(SigningDeviceMsg::WalletRegistered(
            3,
            fingerprint,
            "name".into(),
            Some([0xab; 32]),
        ))
        .expect("a registration event");
        assert!(event.ok, "a registration answers ok");
        assert_eq!(event.hmac, "ab".repeat(32), "hmac of the event");

        let event = to_event(SigningDeviceMsg::WalletRegistered(
            3,
            fingerprint,
            "name".into(),
            None,
        ))
        .expect("a registration event");
        assert!(event.ok, "a registration answers ok");
        assert!(event.hmac.is_empty(), "no hmac, got {:?}", event.hmac);
    }
}
