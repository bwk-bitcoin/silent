//! Configuration module for Silent accounts.
//!
//! Wraps bwk-sp::Config with CXX-compatible interface for C++ bindings.

use std::fs;
use std::path::PathBuf;
use std::str::FromStr;
use std::sync::OnceLock;

use bitcoin::{
    bip32::{ChildNumber, Fingerprint, Xpub},
    secp256k1::Secp256k1,
    Network as BtcNetwork,
};
use bwk_sp::account::config::{Config as SpConfig, SubAccountConfig};
use bwk_sp::bwk::bwk_electrum::raw_client::CertificateCheck;
use bwk_sp::bwk_sign::bwk_descriptor::{
    descriptor::{tr_path, wpkh_path},
    sp_descriptor::SpDescriptor,
};
use bwk_sp::bwk_sign::miniscript::{Descriptor as MiniscriptDescriptor, DescriptorPublicKey};
use serde::{Deserialize, Serialize};

use crate::ffi::Network;

/// Internal Network type for serialization (since CXX enums don't support Serde).
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub(crate) enum NetworkInternal {
    Regtest,
    Signet,
    Testnet,
    Bitcoin,
}

impl From<Network> for NetworkInternal {
    fn from(value: Network) -> Self {
        match value {
            Network::Regtest => NetworkInternal::Regtest,
            Network::Signet => NetworkInternal::Signet,
            Network::Testnet => NetworkInternal::Testnet,
            Network::Bitcoin => NetworkInternal::Bitcoin,
            // An out-of-range repr cannot be constructed from valid CXX usage, but a
            // panic here would cross the FFI boundary. Log and fall back to Signet.
            _ => {
                log::error!("unexpected Network variant, falling back to Signet");
                NetworkInternal::Signet
            }
        }
    }
}

impl From<NetworkInternal> for Network {
    fn from(value: NetworkInternal) -> Self {
        match value {
            NetworkInternal::Regtest => Network::Regtest,
            NetworkInternal::Signet => Network::Signet,
            NetworkInternal::Testnet => Network::Testnet,
            NetworkInternal::Bitcoin => Network::Bitcoin,
        }
    }
}

/// Configuration for a Silent account.
///
/// Wraps bwk-sp::Config with simplified interface for CXX FFI.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    /// Account name
    pub account_name: String,
    /// Bitcoin network (stored as internal type for serde)
    #[serde(with = "network_serde")]
    pub network: Network,
    /// BIP392 Silent Payments descriptor.
    pub descriptor: String,
    /// The key's BIP84 segwit descriptor, its sub-account: empty for none.
    #[serde(default)]
    pub segwit_descriptor: String,
    /// The key's BIP86 taproot descriptor, its sub-account: empty for none.
    #[serde(default)]
    pub taproot_descriptor: String,
    /// Blindbit server URL
    pub blindbit_url: String,
    /// Electrum server address (host:port) for sub-account syncing
    #[serde(default)]
    pub electrum_url: String,
    /// Whether the Electrum server's TLS certificate is checked. Off only to reach a
    /// self-signed server: anyone on the path can then impersonate it.
    #[serde(default = "default_verify_certificate")]
    pub verify_certificate: bool,
    /// Optional dust limit in satoshis
    pub dust_limit: Option<u64>,
    /// Plugin/module identifier that owns this account
    #[serde(default = "default_plugin_id")]
    pub plugin_id: String,
    /// Base directory for account data
    #[serde(skip)]
    pub data_dir: PathBuf,
    /// Runtime-only load error. None when the config was created or loaded successfully.
    #[serde(skip)]
    pub load_error: Option<String>,
}

fn default_plugin_id() -> String {
    "sp".into()
}

fn default_verify_certificate() -> bool {
    true
}

/// The bwk certificate policy for `verify_certificate`.
pub(crate) fn certificate_check(verify_certificate: bool) -> CertificateCheck {
    if verify_certificate {
        CertificateCheck::Validate
    } else {
        CertificateCheck::DangerAcceptInvalid
    }
}

/// Birthday for new mainnet accounts. Scanning from block 1 costs ~958k blocks,
/// and bwk's taproot-activation floor never applies: it only lives in
/// Config::sanitize, which the sp crate never calls.
const MAINNET_BIRTHDAY_HEIGHT: u32 = 899_300;

/// Serde serialization helpers for Network.
pub(crate) mod network_serde {
    use super::*;
    use serde::{Deserialize, Deserializer, Serialize, Serializer};

    pub fn serialize<S>(network: &Network, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        NetworkInternal::from(*network).serialize(serializer)
    }

    pub fn deserialize<'de, D>(deserializer: D) -> Result<Network, D::Error>
    where
        D: Deserializer<'de>,
    {
        NetworkInternal::deserialize(deserializer).map(Network::from)
    }
}

impl Config {
    /// Create a new Config.
    pub fn new(
        account_name: String,
        network: Network,
        descriptor: String,
        blindbit_url: String,
        electrum_url: String,
        dust_limit: Option<u64>,
    ) -> Self {
        let data_dir = datadir();
        Self {
            account_name,
            network,
            descriptor,
            segwit_descriptor: String::new(),
            taproot_descriptor: String::new(),
            blindbit_url,
            electrum_url,
            verify_certificate: default_verify_certificate(),
            dust_limit,
            plugin_id: default_plugin_id(),
            data_dir,
            load_error: None,
        }
    }

    /// Load Config from file.
    pub fn from_file(account_name: String) -> Result<Self, ConfigError> {
        let path = Self::config_path_for_account(&account_name);
        let content = fs::read_to_string(&path)
            .map_err(|e| ConfigError::Io(format!("failed to read config: {e}")))?;
        let mut config: Config = serde_json::from_str(&content)
            .map_err(|e| ConfigError::Parse(format!("failed to parse config: {e}")))?;
        config.account_name = account_name;
        config.data_dir = datadir();
        Ok(config)
    }

    /// Save Config to file.
    pub fn to_file(&self) {
        if let Err(e) = self.write_file() {
            log::error!("Config::to_file() {e}");
        }
    }

    /// Save Config to file, reporting a failed write.
    pub(crate) fn write_file(&self) -> Result<(), String> {
        let path = self.config_path();
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let content =
            serde_json::to_string_pretty(self).map_err(|e| format!("failed to serialize: {e}"))?;
        fs::write(&path, content).map_err(|e| format!("failed to write: {e}"))
    }

    /// Check if the config was loaded successfully.
    pub fn is_ok(&self) -> bool {
        self.load_error.is_none()
    }

    /// Get error message (empty if is_ok).
    pub fn get_error(&self) -> String {
        self.load_error.clone().unwrap_or_default()
    }

    /// Get account directory path.
    pub fn account_dir(&self) -> PathBuf {
        self.data_dir.join(&self.account_name)
    }

    /// Get config file path.
    pub fn config_path(&self) -> PathBuf {
        self.account_dir().join("config.json")
    }

    /// Get config file path for a specific account.
    fn config_path_for_account(account_name: &str) -> PathBuf {
        datadir().join(account_name).join("config.json")
    }

    /// Convert to bwk-sp::Config.
    pub fn to_sp_config(&self) -> Result<SpConfig, String> {
        let descriptor = SpDescriptor::from_str(&self.descriptor)
            .map_err(|e| format!("invalid SP descriptor: {e}"))?;
        let has_spend_secret = descriptor
            .spend_secret_key(&Secp256k1::new())
            .map_err(|e| format!("invalid SP descriptor: {e}"))?
            .is_some();
        if has_spend_secret {
            return Err(crate::keys::error::Error::SpSpendDescriptor.to_string());
        }
        let mut config = SpConfig::from_descriptor(
            self.account_name.clone(),
            self.network.into(),
            descriptor,
            self.blindbit_url.clone(),
            self.data_dir.clone(),
        );
        config.set_dust_limit(self.dust_limit);
        // Only seeds a fresh store: bwk keeps the persisted birthday once scanned.
        let birthday = match self.network {
            Network::Bitcoin => MAINNET_BIRTHDAY_HEIGHT,
            _ => 1,
        };
        config.set_birthday_height(Some(birthday));

        if !self.electrum_url.is_empty() {
            let (electrum_host, electrum_port) = parse_electrum_url(&self.electrum_url);
            if let (Some(host), Some(port)) = (electrum_host, electrum_port) {
                config.set_electrum_endpoint(host, port);
                config.set_certificate_check(certificate_check(self.verify_certificate));
            }
        }

        // The sub-accounts, segwit first: watch-only, the key signs their spends. Each
        // watches the account's Electrum server: bwk keeps a sub-account with no
        // endpoint offline.
        for sub_account in [&self.segwit_descriptor, &self.taproot_descriptor] {
            if sub_account.is_empty() {
                continue;
            }
            let descriptor = MiniscriptDescriptor::<DescriptorPublicKey>::from_str(sub_account)
                .map_err(|e| format!("invalid sub-account descriptor: {e}"))?;
            config.descriptors.push(SubAccountConfig {
                descriptor: descriptor.into(),
                mnemonic: None,
                endpoint: config.endpoint().clone(),
            });
        }

        Ok(config.with_persistence(Some(bwk_sp::bwk::persist::PersistenceKind::default())))
    }

    /// Get SP descriptor.
    pub fn get_descriptor(&self) -> String {
        self.descriptor.clone()
    }

    /// Set SP descriptor.
    pub fn set_descriptor(&mut self, descriptor: String) {
        self.descriptor = descriptor;
    }

    /// The segwit sub-account descriptor, empty for none.
    pub fn get_segwit_descriptor(&self) -> String {
        self.segwit_descriptor.clone()
    }

    /// Set the segwit sub-account descriptor, empty for none.
    pub fn set_segwit_descriptor(&mut self, descriptor: String) {
        self.segwit_descriptor = descriptor;
    }

    /// The taproot sub-account descriptor, empty for none.
    pub fn get_taproot_descriptor(&self) -> String {
        self.taproot_descriptor.clone()
    }

    /// Set the taproot sub-account descriptor, empty for none.
    pub fn set_taproot_descriptor(&mut self, descriptor: String) {
        self.taproot_descriptor = descriptor;
    }

    /// Get network.
    pub fn get_network(&self) -> Network {
        self.network
    }

    /// Set network.
    pub fn set_network(&mut self, network: Network) {
        self.network = network;
    }

    /// Get BlindBit URL.
    pub fn get_blindbit_url(&self) -> String {
        self.blindbit_url.clone()
    }

    /// Set BlindBit URL.
    pub fn set_blindbit_url(&mut self, url: String) {
        self.blindbit_url = url;
    }

    /// Get Electrum URL.
    pub fn get_electrum_url(&self) -> String {
        self.electrum_url.clone()
    }

    /// Set Electrum URL. Another server checks its certificate again: skipping the
    /// check was a choice made for one self-signed server (bwk's `Endpoint::set` does
    /// the same).
    pub fn set_electrum_url(&mut self, url: String) {
        if url != self.electrum_url {
            self.verify_certificate = true;
        }
        self.electrum_url = url;
    }

    /// Whether the Electrum server's TLS certificate is checked.
    pub fn get_verify_certificate(&self) -> bool {
        self.verify_certificate
    }

    /// Check the Electrum server's TLS certificate, or not (a self-signed server).
    pub fn set_verify_certificate(&mut self, verify: bool) {
        self.verify_certificate = verify;
    }

    /// Get dust limit.
    pub fn get_dust_limit(&self) -> u64 {
        self.dust_limit.unwrap_or(0)
    }

    /// Set dust limit.
    pub fn set_dust_limit(&mut self, limit: u64) {
        self.dust_limit = if limit > 0 { Some(limit) } else { None };
    }

    /// Get plugin/module identifier.
    pub fn get_plugin_id(&self) -> String {
        self.plugin_id.clone()
    }

    /// Set plugin/module identifier.
    pub fn set_plugin_id(&mut self, plugin_id: String) {
        self.plugin_id = plugin_id;
    }

    /// The bwk::Account config of a musig account (plugin id "musig").
    ///
    /// The bwk stores sit next to this `config.json` (`dir_name` empty), and bwk's
    /// default config store writes nothing, so it never overwrites this file.
    pub fn to_musig_config(&self) -> Result<bwk::config::Config, String> {
        if self.plugin_id != crate::musig::MUSIG_PLUGIN_ID {
            return Err(format!(
                "not a musig account: plugin id is '{}'",
                self.plugin_id
            ));
        }
        let mut config = bwk::config::Config::new_from_descriptor(
            None,
            self.account_name.clone(),
            self.network.into(),
            &self.descriptor,
            self.data_dir.clone(),
            String::new(),
            Some(bwk::persist::PersistenceKind::default()),
        )
        .map_err(|e| format!("invalid musig descriptor: {e}"))?;

        // An empty or invalid url leaves the endpoint unset: the account stays offline.
        if !self.electrum_url.is_empty() {
            if let (Some(host), Some(port)) = parse_electrum_url(&self.electrum_url) {
                config.scanner.set_electrum(Some(host), Some(port));
                config
                    .scanner
                    .set_certificate_check(certificate_check(self.verify_certificate));
            }
        }

        Ok(config)
    }
}

/// Override for the data directory. Once set, all calls to `datadir()` use this path.
static DATADIR_OVERRIDE: OnceLock<PathBuf> = OnceLock::new();

/// Override the data directory. Intended for testing.
/// Must be called before any other config operations. Can only be set once.
pub fn set_datadir(path: PathBuf) {
    if DATADIR_OVERRIDE.set(path).is_err() {
        log::error!("set_datadir: datadir already set");
    }
}

/// Returns the base data directory for Silent.
///
/// On Linux: ~/.silent
/// On other systems: {config_dir}/Silent
pub fn datadir() -> PathBuf {
    if let Some(dir) = DATADIR_OVERRIDE.get() {
        maybe_create_dir(dir);
        return dir.clone();
    }
    #[cfg(target_os = "linux")]
    let dir = {
        let mut dir = match dirs::home_dir() {
            Some(d) => d,
            None => {
                log::error!("datadir: home directory not found, falling back to /tmp");
                PathBuf::from("/tmp")
            }
        };
        dir.push(".silent");
        dir
    };

    #[cfg(not(target_os = "linux"))]
    let dir = {
        let mut dir = match dirs::config_dir() {
            Some(d) => d,
            None => {
                log::error!("datadir: config directory not found, falling back to /tmp");
                PathBuf::from("/tmp")
            }
        };
        dir.push("Silent");
        dir
    };

    maybe_create_dir(&dir);
    dir
}

/// Create directory if it doesn't exist.
fn maybe_create_dir(dir: &PathBuf) {
    if !dir.exists() {
        #[cfg(unix)]
        {
            use std::fs::DirBuilder;
            use std::os::unix::fs::DirBuilderExt;

            let mut builder = DirBuilder::new();
            if let Err(e) = builder.mode(0o700).recursive(true).create(dir) {
                log::error!("failed to create directory {}: {e}", dir.display());
            }
        }

        #[cfg(not(unix))]
        if let Err(e) = fs::create_dir_all(dir) {
            log::error!("failed to create directory {}: {e}", dir.display());
        }
    }
}

/// Configuration errors.
#[derive(Debug, thiserror::Error)]
pub enum ConfigError {
    #[error("io error: {0}")]
    Io(String),
    #[error("parse error: {0}")]
    Parse(String),
}

/// Split "[ssl://]host:port" into the address bwk expects and its port.
///
/// An SSL address keeps its `ssl://` prefix on the host: bwk's account config has
/// no scheme field, and `bwk_electrum::client::Client::new` reads the scheme off
/// that prefix. An unsupported scheme yields no endpoint, leaving electrum off.
pub(crate) fn parse_electrum_url(url: &str) -> (Option<String>, Option<u16>) {
    let (host, port, scheme) = match bwk_sp::bwk::bwk_electrum::url::parse_electrum_url(url) {
        Ok(parsed) => parsed,
        Err(e) => {
            log::error!("parse_electrum_url: invalid electrum url '{url}': {e}");
            return (None, None);
        }
    };
    let host = host.map(|host| match scheme {
        bwk_sp::bwk::bwk_electrum::url::ElectrumScheme::Ssl => format!("ssl://{host}"),
        bwk_sp::bwk::bwk_electrum::url::ElectrumScheme::Tcp => host,
    });
    (host, port)
}

// CXX FFI functions

/// Create a new config.
pub fn new_config(
    account_name: String,
    network: Network,
    descriptor: String,
    blindbit_url: String,
    electrum_url: String,
    dust_limit: u64,
    plugin_id: String,
) -> Box<Config> {
    let dust = if dust_limit > 0 {
        Some(dust_limit)
    } else {
        None
    };
    let mut config = Config::new(
        account_name,
        network,
        descriptor,
        blindbit_url,
        electrum_url,
        dust,
    );
    config.set_plugin_id(plugin_id);
    Box::new(config)
}

/// The BIP84 segwit descriptor of the key `fingerprint`, from its xpub at
/// `84'/coin'/0'`, as the account's segwit sub-account. Empty when the fingerprint or
/// the xpub is invalid.
pub fn segwit_descriptor_for(fingerprint: String, xpub: String, network: Network) -> String {
    sub_account_descriptor("wpkh", &fingerprint, &xpub, network)
}

/// The BIP86 taproot descriptor of the key `fingerprint`, from its xpub at
/// `86'/coin'/0'`, as the account's taproot sub-account. Empty when the fingerprint or
/// the xpub is invalid.
pub fn taproot_descriptor_for(fingerprint: String, xpub: String, network: Network) -> String {
    sub_account_descriptor("tr", &fingerprint, &xpub, network)
}

/// The account a taproot / segwit sub-account is derived at.
pub(crate) fn sub_account() -> ChildNumber {
    ChildNumber::from_hardened_idx(0).expect("account 0 is a valid index")
}

/// `wpkh` or `tr` over the account 0 path of that script type, as bwk builds its own
/// (`bwk_descriptor::descriptor::{wpkh, tr}`), receive and change in one.
fn sub_account_descriptor(script: &str, fingerprint: &str, xpub: &str, network: Network) -> String {
    let account = sub_account();
    let path = match script {
        "wpkh" => wpkh_path(network.into(), account),
        _ => tr_path(network.into(), account),
    };
    let built = path.map_err(|e| e.to_string()).and_then(|path| {
        let fingerprint = Fingerprint::from_str(fingerprint).map_err(|e| e.to_string())?;
        let xpub = Xpub::from_str(xpub).map_err(|e| e.to_string())?;
        MiniscriptDescriptor::<DescriptorPublicKey>::from_str(&format!(
            "{script}([{fingerprint}/{path}]{xpub}/<0;1>/*)"
        ))
        .map_err(|e| e.to_string())
    });
    match built {
        Ok(descriptor) => descriptor.to_string(),
        Err(e) => {
            log::error!("sub_account_descriptor {script}: {e}");
            String::new()
        }
    }
}

/// Delete an account's data from disk.
pub fn delete_config(account_name: String) -> bool {
    let data_dir = datadir();
    match SpConfig::delete_account_dir(&data_dir, &account_name) {
        Ok(()) => true,
        Err(e) => {
            log::error!("failed to delete account '{account_name}': {e}");
            false
        }
    }
}

/// Load config from file.
pub fn config_from_file(account_name: String) -> Box<Config> {
    match Config::from_file(account_name.clone()) {
        Ok(config) => Box::new(config),
        Err(e) => {
            let error = format!("failed to load config: {e}");
            log::error!("config_from_file: {error}");
            // Return a config marked not-ok. Check is_ok() before use.
            let mut config = Config::new(
                account_name,
                Network::Signet,
                String::new(),
                String::new(),
                String::new(),
                None,
            );
            config.load_error = Some(error);
            Box::new(config)
        }
    }
}

/// List all config directories.
pub fn list_configs() -> Vec<String> {
    let path = datadir();
    let mut out = vec![];
    if let Ok(folders) = fs::read_dir(path) {
        for entry in folders.flatten() {
            if let Ok(md) = entry.metadata() {
                if md.is_dir() {
                    let acc_name = entry.file_name().to_string_lossy().to_string();
                    if Config::from_file(acc_name.clone()).is_ok() {
                        out.push(acc_name);
                    }
                }
            }
        }
    }
    out
}

impl From<Network> for BtcNetwork {
    fn from(value: Network) -> Self {
        match value {
            Network::Regtest => BtcNetwork::Regtest,
            Network::Signet => BtcNetwork::Signet,
            Network::Testnet => BtcNetwork::Testnet,
            Network::Bitcoin => BtcNetwork::Bitcoin,
            // SAFETY: Network enum is defined in CXX bridge with exactly 4 variants.
            // CXX guarantees type safety - invalid values cannot be constructed.
            // This wildcard exists only for exhaustiveness checking.
            _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
        }
    }
}

impl From<BtcNetwork> for Network {
    fn from(value: BtcNetwork) -> Self {
        match value {
            BtcNetwork::Regtest => Network::Regtest,
            BtcNetwork::Signet => Network::Signet,
            BtcNetwork::Testnet => Network::Testnet,
            BtcNetwork::Bitcoin => Network::Bitcoin,
            // SAFETY: bitcoin::Network only has 4 variants that we explicitly handle.
            // This wildcard exists only for exhaustiveness checking.
            _ => unreachable!("bitcoin::Network enum only has 4 variants"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_deserialize_without_plugin_id_defaults_to_sp() {
        let json = r#"{
            "account_name":"test_account",
            "network":"Signet",
            "descriptor":"sp(L4rK1yDtCWekvXuE6oXD9jCYfFNV2cWRpVuPLBcCU2z8TrisoyY1,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600)",
            "blindbit_url":"http://localhost:50001",
            "electrum_url":"",
            "dust_limit":546
        }"#;

        let config: Config = serde_json::from_str(json).expect("config json should deserialize");
        assert_eq!(config.plugin_id, "sp");
        // A config written before the sub-account descriptors has none.
        assert!(config.segwit_descriptor.is_empty());
        assert!(config.taproot_descriptor.is_empty());
    }

    /// A key's fingerprint and its xpub at `path`, from a fixed seed.
    fn key_xpub(path: &str) -> (String, String) {
        use bitcoin::bip32::{DerivationPath, Xpriv};
        let secp = Secp256k1::new();
        let master = Xpriv::new_master(BtcNetwork::Regtest, &[7u8; 32]).unwrap();
        let derived = master
            .derive_priv(&secp, &DerivationPath::from_str(path).unwrap())
            .unwrap();
        (
            master.fingerprint(&secp).to_string(),
            Xpub::from_priv(&secp, &derived).to_string(),
        )
    }

    #[test]
    fn builds_the_segwit_and_taproot_descriptors_of_a_key() {
        let (fingerprint, xpub) = key_xpub("m/84'/1'/0'");
        let segwit = segwit_descriptor_for(fingerprint.clone(), xpub.clone(), Network::Regtest);
        assert!(
            segwit.starts_with(&format!("wpkh([{fingerprint}/84'/1'/0']{xpub}/<0;1>/*)")),
            "{segwit}"
        );

        let (fingerprint, xpub) = key_xpub("m/86'/1'/0'");
        let taproot = taproot_descriptor_for(fingerprint.clone(), xpub.clone(), Network::Regtest);
        assert!(
            taproot.starts_with(&format!("tr([{fingerprint}/86'/1'/0']{xpub}/<0;1>/*)")),
            "{taproot}"
        );
    }

    #[test]
    fn an_invalid_key_builds_no_descriptor() {
        let (fingerprint, xpub) = key_xpub("m/84'/1'/0'");
        assert!(segwit_descriptor_for("nothex".into(), xpub, Network::Regtest).is_empty());
        assert!(
            taproot_descriptor_for(fingerprint, "xpubnope".into(), Network::Regtest).is_empty()
        );
    }

    #[test]
    fn each_sub_account_descriptor_adds_a_sub_account() {
        let (fingerprint, segwit_xpub) = key_xpub("m/84'/1'/0'");
        let (_, taproot_xpub) = key_xpub("m/86'/1'/0'");
        let mut config = config_for(Network::Regtest);
        assert_eq!(config.to_sp_config().unwrap().descriptors.len(), 0);

        config.set_segwit_descriptor(segwit_descriptor_for(
            fingerprint.clone(),
            segwit_xpub,
            Network::Regtest,
        ));
        assert_eq!(config.to_sp_config().unwrap().descriptors.len(), 1);

        config.set_taproot_descriptor(taproot_descriptor_for(
            fingerprint,
            taproot_xpub,
            Network::Regtest,
        ));
        assert_eq!(config.to_sp_config().unwrap().descriptors.len(), 2);

        config.set_taproot_descriptor("tr(garbage)".into());
        assert!(config.to_sp_config().is_err());
    }

    #[test]
    fn sub_accounts_watch_the_account_electrum_server() {
        let (fingerprint, xpub) = key_xpub("m/84'/1'/0'");
        let mut config = config_for(Network::Regtest);
        config.set_segwit_descriptor(segwit_descriptor_for(fingerprint, xpub, Network::Regtest));
        config.set_electrum_url("127.0.0.1:50001".to_string());
        let sp_config = config.to_sp_config().unwrap();
        assert!(sp_config.endpoint().url().is_some());
        assert_eq!(&sp_config.descriptors[0].endpoint, sp_config.endpoint());
    }

    const TEST_DESCRIPTOR: &str =
        "sp(L4rK1yDtCWekvXuE6oXD9jCYfFNV2cWRpVuPLBcCU2z8TrisoyY1,0260b2003c386519fc9eadf2b5cf124dd8eea4c4e68d5e154050a9346ea98ce600)";

    fn config_for(network: Network) -> Config {
        Config::new(
            "test_birthday".to_string(),
            network,
            TEST_DESCRIPTOR.to_string(),
            "http://localhost:8000".to_string(),
            String::new(),
            Some(546),
        )
    }

    #[test]
    fn mainnet_birthday_skips_the_pre_sp_chain() {
        assert_eq!(
            config_for(Network::Bitcoin)
                .to_sp_config()
                .unwrap()
                .birthday_height,
            Some(MAINNET_BIRTHDAY_HEIGHT)
        );
    }

    #[test]
    fn test_networks_scan_from_the_first_block() {
        // A birthday above a short regtest chain would scan past the tip.
        for network in [Network::Regtest, Network::Signet, Network::Testnet] {
            assert_eq!(
                config_for(network).to_sp_config().unwrap().birthday_height,
                Some(1),
                "network {network:?} must scan from block 1"
            );
        }
    }

    #[test]
    fn parse_electrum_url_keeps_ssl_prefix_on_host() {
        // bwk's Client::new reads the scheme off the host prefix.
        assert_eq!(
            parse_electrum_url("ssl://electrum.pythcoiner.dev:50002"),
            (
                Some("ssl://electrum.pythcoiner.dev".to_string()),
                Some(50002)
            )
        );
    }

    #[test]
    fn parse_electrum_url_leaves_plaintext_bare() {
        assert_eq!(
            parse_electrum_url("localhost:50001"),
            (Some("localhost".to_string()), Some(50001))
        );
        assert_eq!(
            parse_electrum_url("tcp://localhost:50001"),
            (Some("localhost".to_string()), Some(50001))
        );
    }

    #[test]
    fn parse_electrum_url_empty_is_offline() {
        assert_eq!(parse_electrum_url(""), (None, None));
    }

    #[test]
    fn parse_electrum_url_unsupported_scheme_is_offline() {
        assert_eq!(parse_electrum_url("wss://host:443"), (None, None));
    }
}
