//! Key record model for Silent's hardware/software key registry.
//!
//! A key is identified by its master fingerprint, one record per fingerprint, and
//! records are scoped per network: the same seed on testnet and on mainnet are two
//! separate records.

pub mod crypto;
pub mod error;
pub mod hot;
pub mod manager;
pub mod psbt;
pub mod qr;
pub mod record;
pub mod sdcard;
pub mod store;

pub use crypto::Password;
pub use error::Error;
pub use hot::{generate_mnemonic, MNEMONIC_WORDS_LONG, MNEMONIC_WORDS_SHORT};
pub use manager::{KeyManager, KeyManagerImpl, LiveDevice};
pub use psbt::{inspect, PsbtArtifacts};
pub use record::{
    DescriptorEntry, DeviceRecord, KeyRecord, RouteKind, Routes, SecretEnvelope, XpubEntry,
};
pub use store::KeyStore;

/// Bump when `KeyRecord`'s on-disk shape changes in a way older Silent versions cannot
/// read. `KeyRecord::check_version` refuses to load anything greater than this.
pub const KEY_RECORD_VERSION: u32 = 1;

/// Directory name (under the account/module data dir) where key records are stored.
pub const KEYS_DIR_NAME: &str = "keys";

/// The network name used both for directory layout and as crypto associated data, so
/// the two always agree.
pub fn network_name(network: bitcoin::Network) -> Result<&'static str, Error> {
    match network {
        bitcoin::Network::Bitcoin => Ok("bitcoin"),
        bitcoin::Network::Testnet => Ok("testnet"),
        bitcoin::Network::Signet => Ok("signet"),
        bitcoin::Network::Regtest => Ok("regtest"),
        _ => Err(Error::UnsupportedNetwork(network.to_string())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn network_name_covers_supported_networks() {
        assert_eq!(
            network_name(bitcoin::Network::Bitcoin).expect("bitcoin is supported"),
            "bitcoin"
        );
        assert_eq!(
            network_name(bitcoin::Network::Testnet).expect("testnet is supported"),
            "testnet"
        );
        assert_eq!(
            network_name(bitcoin::Network::Signet).expect("signet is supported"),
            "signet"
        );
        assert_eq!(
            network_name(bitcoin::Network::Regtest).expect("regtest is supported"),
            "regtest"
        );
    }
}
