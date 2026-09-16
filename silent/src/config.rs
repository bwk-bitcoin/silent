//! Configuration module for Silent accounts.
//!
//! Wraps bwk-sp::Config with CXX-compatible interface for C++ bindings.

use std::fs;
use std::path::PathBuf;
use std::str::FromStr;
use std::sync::OnceLock;

use bitcoin::{secp256k1::Secp256k1, Network as BtcNetwork};
use bwk_sp::account::config::Config as SpConfig;
use bwk_sp::bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor;
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
    /// Blindbit server URL
    pub blindbit_url: String,
    /// Electrum server address (host:port) for sub-account syncing
    #[serde(default)]
    pub electrum_url: String,
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
            blindbit_url,
            electrum_url,
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
        let path = self.config_path();
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }

        match serde_json::to_string_pretty(self) {
            Ok(content) => {
                if let Err(e) = fs::write(&path, content) {
                    log::error!("Config::to_file() failed to write: {e}");
                }
            }
            Err(e) => log::error!("Config::to_file() failed to serialize: {e}"),
        }
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
            }
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

    /// Set Electrum URL.
    pub fn set_electrum_url(&mut self, url: String) {
        self.electrum_url = url;
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
