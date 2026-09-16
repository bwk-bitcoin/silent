use bitcoin::{
    bip32::{DerivationPath, Fingerprint, Xpub},
    Network,
};
use serde::{Deserialize, Serialize};

use crate::keys::{error::Error, KEY_RECORD_VERSION};

/// Index the hot route must occupy when present. Named so the invariant reads the
/// same everywhere it is checked.
const HOT_ROUTE_INDEX: usize = 0;

/// A signing route a key can be reached through.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RouteKind {
    Hot,
    Usb,
    Qr,
    SdCard,
}

impl RouteKind {
    /// The wire and display name, kept as the single source for both so they cannot
    /// drift apart.
    pub fn as_str(self) -> &'static str {
        match self {
            RouteKind::Hot => "hot",
            RouteKind::Usb => "usb",
            RouteKind::Qr => "qr",
            RouteKind::SdCard => "sd_card",
        }
    }
}

impl std::fmt::Display for RouteKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// The set of routes a key is reachable through. Enforces two invariants on every
/// path in and out, including deserialization: a kind appears at most once, and the
/// hot route, when present, is always first.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(try_from = "Vec<RouteKind>", into = "Vec<RouteKind>")]
pub struct Routes(Vec<RouteKind>);

impl TryFrom<Vec<RouteKind>> for Routes {
    type Error = Error;

    fn try_from(kinds: Vec<RouteKind>) -> Result<Self, Self::Error> {
        for (index, kind) in kinds.iter().enumerate() {
            if kinds[..index].contains(kind) {
                return Err(Error::DuplicateRoute(*kind));
            }
        }
        if let Some(index) = kinds.iter().position(|kind| *kind == RouteKind::Hot) {
            if index != HOT_ROUTE_INDEX {
                return Err(Error::HotRouteNotFirst);
            }
        }
        Ok(Routes(kinds))
    }
}

impl From<Routes> for Vec<RouteKind> {
    fn from(routes: Routes) -> Self {
        routes.0
    }
}

impl Routes {
    pub fn as_slice(&self) -> &[RouteKind] {
        &self.0
    }

    pub fn contains(&self, kind: RouteKind) -> bool {
        self.0.contains(&kind)
    }

    pub fn first(&self) -> Option<RouteKind> {
        self.0.first().copied()
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// Hot goes to the front, every other kind to the back. A kind already present is
    /// `Error::DuplicateRoute`.
    pub fn push(&mut self, kind: RouteKind) -> Result<(), Error> {
        if self.0.contains(&kind) {
            return Err(Error::DuplicateRoute(kind));
        }
        if kind == RouteKind::Hot {
            self.0.insert(HOT_ROUTE_INDEX, kind);
        } else {
            self.0.push(kind);
        }
        Ok(())
    }

    /// `Error::RouteNotFound` when the kind is absent.
    pub fn remove(&mut self, kind: RouteKind) -> Result<(), Error> {
        let position = self
            .0
            .iter()
            .position(|existing| *existing == kind)
            .ok_or(Error::RouteNotFound(kind))?;
        self.0.remove(position);
        Ok(())
    }

    /// The new order must be a permutation of the current set, otherwise
    /// `Error::RouteOrderMismatch`. The hot-first invariant still applies.
    pub fn set_order(&mut self, order: Vec<RouteKind>) -> Result<(), Error> {
        let reordered = Routes::try_from(order)?;
        let is_permutation = reordered.0.len() == self.0.len()
            && self.0.iter().all(|kind| reordered.contains(*kind));
        if !is_permutation {
            return Err(Error::RouteOrderMismatch);
        }
        self.0 = reordered.0;
        Ok(())
    }
}

/// A device a key's hot route or hardware routes were seen on. Identity is the tuple
/// of these four fields (`same_device`): the key record it lives in is already the
/// fingerprint, so there is no fingerprint field here.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceRecord {
    pub kind: String,
    pub model: String,
    pub serial: String,
    pub module: String,
}

impl DeviceRecord {
    pub fn same_device(&self, other: &DeviceRecord) -> bool {
        self.kind == other.kind
            && self.model == other.model
            && self.serial == other.serial
            && self.module == other.module
    }
}

/// A cached extended public key at a derivation path.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct XpubEntry {
    pub path: DerivationPath,
    pub xpub: Xpub,
}

/// A descriptor this key participates in. Presence in `KeyRecord::descriptors` is the
/// registration: there is no separate `registered` flag to keep in sync.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DescriptorEntry {
    pub alias: String,
    pub descriptor: String,
}

/// An encrypted secret at rest. This task only declares the shape; a later task fills
/// and opens it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SecretEnvelope {
    pub version: u32,
    pub kdf: String,
    pub m_cost_kib: u32,
    pub t_cost: u32,
    pub p_cost: u32,
    pub salt: String,
    pub cipher: String,
    pub nonce: String,
    pub ciphertext: String,
}

/// A persisted key, identified by its master fingerprint. Keys are per network: the
/// same seed on testnet and on mainnet are two separate records.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct KeyRecord {
    pub version: u32,
    pub fingerprint: Fingerprint,
    pub network: Network,
    pub alias: String,
    #[serde(default)]
    pub routes: Routes,
    #[serde(default)]
    pub devices: Vec<DeviceRecord>,
    #[serde(default)]
    pub xpubs: Vec<XpubEntry>,
    #[serde(default)]
    pub descriptors: Vec<DescriptorEntry>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub secret: Option<SecretEnvelope>,
}

impl KeyRecord {
    pub fn new(fingerprint: Fingerprint, network: Network, alias: String) -> Self {
        Self {
            version: KEY_RECORD_VERSION,
            fingerprint,
            network,
            alias,
            routes: Routes::default(),
            devices: Vec::new(),
            xpubs: Vec::new(),
            descriptors: Vec::new(),
            secret: None,
        }
    }

    /// A record from a newer Silent must never be silently accepted.
    pub fn check_version(&self) -> Result<(), Error> {
        if self.version > KEY_RECORD_VERSION {
            return Err(Error::UnsupportedRecordVersion {
                found: self.version,
                supported: KEY_RECORD_VERSION,
            });
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use super::*;

    fn fingerprint(value: u32) -> Fingerprint {
        Fingerprint::from(value.to_be_bytes())
    }

    fn test_xpub() -> Xpub {
        let seed = [7u8; 32];
        let xpriv = bitcoin::bip32::Xpriv::new_master(bitcoin::NetworkKind::Test, &seed)
            .expect("valid seed");
        let secp = bitcoin::secp256k1::Secp256k1::new();
        Xpub::from_priv(&secp, &xpriv)
    }

    #[test]
    fn routes_push_rejects_a_duplicate_kind() {
        let mut routes = Routes::default();
        routes.push(RouteKind::Usb).expect("first push succeeds");
        let result = routes.push(RouteKind::Usb);
        assert!(
            matches!(result, Err(Error::DuplicateRoute(RouteKind::Usb))),
            "expected DuplicateRoute(Usb), got {result:?}"
        );
        assert_eq!(
            routes.as_slice(),
            &[RouteKind::Usb],
            "list should be unchanged after the rejected push, got {:?}",
            routes.as_slice()
        );
    }

    #[test]
    fn routes_push_puts_hot_first() {
        let mut routes = Routes::default();
        routes.push(RouteKind::Usb).expect("push usb");
        routes.push(RouteKind::Qr).expect("push qr");
        routes.push(RouteKind::Hot).expect("push hot");
        assert_eq!(
            routes.as_slice(),
            &[RouteKind::Hot, RouteKind::Usb, RouteKind::Qr],
            "hot should be moved to the front, got {:?}",
            routes.as_slice()
        );
    }

    #[test]
    fn routes_remove_reports_a_missing_kind() {
        let mut routes = Routes::try_from(vec![RouteKind::Usb]).expect("valid routes");
        let result = routes.remove(RouteKind::SdCard);
        assert!(
            matches!(result, Err(Error::RouteNotFound(RouteKind::SdCard))),
            "expected RouteNotFound(SdCard), got {result:?}"
        );
    }

    #[test]
    fn routes_set_order_rejects_a_non_permutation() {
        let mut routes =
            Routes::try_from(vec![RouteKind::Usb, RouteKind::Qr]).expect("valid routes");
        let shorter = routes.set_order(vec![RouteKind::Usb]);
        assert!(
            matches!(shorter, Err(Error::RouteOrderMismatch)),
            "expected RouteOrderMismatch for a shorter order, got {shorter:?}"
        );
        let longer = routes.set_order(vec![RouteKind::Usb, RouteKind::Qr, RouteKind::SdCard]);
        assert!(
            matches!(longer, Err(Error::RouteOrderMismatch)),
            "expected RouteOrderMismatch for a longer order, got {longer:?}"
        );
    }

    #[test]
    fn routes_set_order_rejects_hot_out_of_front() {
        let mut routes =
            Routes::try_from(vec![RouteKind::Hot, RouteKind::Usb]).expect("valid routes");
        let result = routes.set_order(vec![RouteKind::Usb, RouteKind::Hot]);
        assert!(
            matches!(result, Err(Error::HotRouteNotFirst)),
            "expected HotRouteNotFirst, got {result:?}"
        );
    }

    #[test]
    fn routes_deserialize_rejects_a_broken_list() {
        let duplicate = serde_json::from_str::<Routes>(r#"["usb","usb"]"#);
        assert!(
            duplicate.is_err(),
            "expected a duplicate route list to fail deserialization, got {duplicate:?}"
        );
        let hot_out_of_front = serde_json::from_str::<Routes>(r#"["usb","hot"]"#);
        assert!(
            hot_out_of_front.is_err(),
            "expected hot out of front to fail deserialization, got {hot_out_of_front:?}"
        );
    }

    #[test]
    fn routes_round_trip_through_json() {
        let routes = Routes::try_from(vec![RouteKind::Hot, RouteKind::Usb, RouteKind::Qr])
            .expect("valid routes");
        let json = serde_json::to_string(&routes).expect("serialize routes");
        assert_eq!(
            json, r#"["hot","usb","qr"]"#,
            "unexpected serialized routes: {json}"
        );
        let round_tripped: Routes = serde_json::from_str(&json).expect("deserialize routes");
        assert_eq!(
            round_tripped, routes,
            "round tripped routes {round_tripped:?} differ from original {routes:?}"
        );
    }

    #[test]
    fn key_record_round_trips_through_json() {
        let mut record = KeyRecord::new(fingerprint(1), Network::Signet, "alias".to_string());
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

        let json = serde_json::to_string(&record).expect("serialize key record");
        assert!(
            !json.contains("secret"),
            "serialized record should omit an absent secret, got {json}"
        );
        let round_tripped: KeyRecord = serde_json::from_str(&json).expect("deserialize record");
        assert_eq!(
            round_tripped, record,
            "round tripped record {round_tripped:?} differs from original {record:?}"
        );
    }

    #[test]
    fn key_record_refuses_a_newer_version() {
        let mut record = KeyRecord::new(fingerprint(2), Network::Bitcoin, "alias".to_string());
        record.version = KEY_RECORD_VERSION + 1;
        let result = record.check_version();
        assert!(
            matches!(
                result,
                Err(Error::UnsupportedRecordVersion { found, supported })
                    if found == KEY_RECORD_VERSION + 1 && supported == KEY_RECORD_VERSION
            ),
            "expected UnsupportedRecordVersion, got {result:?}"
        );
    }
}
