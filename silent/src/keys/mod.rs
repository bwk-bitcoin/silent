//! Key record model for Silent's hardware/software key registry.
//!
//! A key is identified by its master fingerprint, one record per fingerprint, and
//! records are scoped per network: the same seed on testnet and on mainnet are two
//! separate records.

pub mod bridge;
pub mod crypto;
pub mod error;
pub mod hot;
pub mod manager;
pub mod psbt;
pub mod qr;
pub mod record;
pub mod sdcard;
pub mod store;

pub use bridge::{new_keys_module, KeysModule};
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

/// The capability bits a device reports, a USB one and a QR one alike: those of the
/// bwk-qr protocol's `CAPABILITIES` field (ENCODING.md).
pub const CAPABILITY_SILENT_PAYMENTS: u32 = 1 << 2;
pub const CAPABILITY_MUSIG: u32 = 1 << 3;

/// The capability bits of a device bwk-hwi lists.
pub fn hwi_capabilities(capabilities: bwk_hwi::service::Capabilities) -> u32 {
    let mut bits = 0;
    if capabilities.silent_payments {
        bits |= CAPABILITY_SILENT_PAYMENTS;
    }
    if capabilities.musig {
        bits |= CAPABILITY_MUSIG;
    }
    bits
}

/// Whether `capabilities` include silent payments.
pub fn supports_silent_payments(capabilities: u32) -> bool {
    capabilities & CAPABILITY_SILENT_PAYMENTS != 0
}

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
    fn hwi_capabilities_maps_to_the_bwk_qr_bits() {
        for (silent_payments, musig, bits) in [
            (false, false, 0),
            (true, false, CAPABILITY_SILENT_PAYMENTS),
            (false, true, CAPABILITY_MUSIG),
            (true, true, CAPABILITY_SILENT_PAYMENTS | CAPABILITY_MUSIG),
        ] {
            let mapped = hwi_capabilities(bwk_hwi::service::Capabilities {
                silent_payments,
                musig,
            });
            assert_eq!(
                mapped, bits,
                "silent_payments {silent_payments}, musig {musig}: expected {bits:#x}, got {mapped:#x}"
            );
            assert_eq!(
                supports_silent_payments(mapped),
                silent_payments,
                "supports_silent_payments({mapped:#x})"
            );
        }
    }

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
