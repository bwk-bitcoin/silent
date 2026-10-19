//! Persisted settings for the HWI module: the service network and the BitBox02 pairing.
//!
//! Laid out the way liana lays out its global settings, one JSON file with a `bitbox`
//! section. Writes are a locked read-modify-write because `store_config` runs on the
//! service's device thread during pairing while the GUI thread may be writing the
//! network, and a plain load-then-save would drop one of the two.

use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

pub use bwk_hwi::bitbox::api::noise::{NoiseConfig, NoiseConfigData};
use bwk_hwi::bitbox::api::{noise::ConfigError, util::Threading};
use fs2::FileExt;
use serde::{Deserialize, Serialize};

use crate::config::{datadir, network_serde};
use crate::ffi::Network;
use crate::state_crypt;

pub(crate) const FILE_NAME: &str = "hwi.json";

/// BitBox02 pairing material: the app's static key plus the paired devices' pubkeys.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BitboxSettings {
    pub noise_config: NoiseConfigData,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct HwiConfig {
    #[serde(with = "network_serde")]
    pub network: Network,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub bitbox: Option<BitboxSettings>,
}

impl Default for HwiConfig {
    fn default() -> Self {
        Self {
            network: Network::Bitcoin,
            bitbox: None,
        }
    }
}

impl HwiConfig {
    pub fn path() -> PathBuf {
        datadir().join(FILE_NAME)
    }

    pub fn load() -> Self {
        let path = Self::path();
        match state_crypt::read_state(&path) {
            Ok(Some(content)) => Self::parse(&content, &path),
            Ok(None) => Self::default(),
            Err(e) => {
                log::error!("HwiConfig::load() {e}");
                Self::default()
            }
        }
    }

    fn parse(content: &str, path: &Path) -> Self {
        if content.trim().is_empty() {
            return Self::default();
        }
        match serde_json::from_str(content) {
            Ok(config) => config,
            Err(e) => {
                log::error!("HwiConfig::load() failed to parse {}: {e}", path.display());
                Self::default()
            }
        }
    }

    /// Read, apply, write back, holding an exclusive lock for the whole cycle so a
    /// concurrent write from the other thread is not lost.
    pub fn update<F>(mut apply: F) -> Result<(), String>
    where
        F: FnMut(&mut HwiConfig),
    {
        let path = Self::path();
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&path)
            .map_err(|e| format!("opening {}: {e}", path.display()))?;
        file.lock_exclusive()
            .map_err(|e| format!("locking {}: {e}", path.display()))?;

        let mut sealed = Vec::new();
        let read = file
            .read_to_end(&mut sealed)
            .map_err(|e| format!("reading {}: {e}", path.display()))
            .and_then(|_| {
                if sealed.is_empty() {
                    Ok(String::new())
                } else if !state_crypt::is_sealed(&sealed) {
                    Err(format!("{} is not encrypted", path.display()))
                } else {
                    state_crypt::open(&sealed).map_err(|e| format!("{}: {e}", path.display()))
                }
            });
        let content = match read {
            Ok(content) => content,
            Err(e) => {
                let _ = FileExt::unlock(&file);
                return Err(e);
            }
        };

        let mut config = Self::parse(&content, &path);
        apply(&mut config);

        let result = Self::write_locked(&mut file, &config, &path);
        let _ = FileExt::unlock(&file);
        result
    }

    fn write_locked(
        file: &mut std::fs::File,
        config: &HwiConfig,
        path: &Path,
    ) -> Result<(), String> {
        let text = serde_json::to_string_pretty(config)
            .map_err(|e| format!("serializing {}: {e}", path.display()))?;
        let content = state_crypt::seal(&text)?;
        file.seek(SeekFrom::Start(0))
            .map_err(|e| format!("seeking {}: {e}", path.display()))?;
        file.write_all(&content)
            .map_err(|e| format!("writing {}: {e}", path.display()))?;
        file.set_len(content.len() as u64)
            .map_err(|e| format!("truncating {}: {e}", path.display()))
    }
}

/// Stores the BitBox02 pairing in the module's config file, so a device pairs once
/// instead of once per run.
pub struct PersistedBitboxNoiseConfig;

impl Threading for PersistedBitboxNoiseConfig {}

impl NoiseConfig for PersistedBitboxNoiseConfig {
    fn read_config(&self) -> Result<NoiseConfigData, ConfigError> {
        Ok(HwiConfig::load()
            .bitbox
            .map(|bitbox| bitbox.noise_config)
            .unwrap_or_default())
    }

    fn store_config(&self, conf: &NoiseConfigData) -> Result<(), ConfigError> {
        HwiConfig::update(|config| {
            config.bitbox = Some(BitboxSettings {
                noise_config: conf.clone(),
            });
        })
        .map_err(ConfigError)
    }
}

pub fn hwi_config_network() -> Network {
    HwiConfig::load().network
}

pub fn hwi_set_config_network(network: Network) {
    if let Err(e) = HwiConfig::update(|config| config.network = network) {
        log::error!("hwi_set_config_network() failed: {e}");
    }
}
