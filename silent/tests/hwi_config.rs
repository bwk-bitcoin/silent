//! HWI module config: the service network and the BitBox02 pairing share one file, and
//! they are written from different threads, so neither write may clobber the other.

use silent::{
    hwi_config_network, hwi_set_config_network, set_datadir, HwiConfig, Network, NoiseConfig,
    NoiseConfigData, PersistedBitboxNoiseConfig,
};
use temp_dir::TempDir;

fn pairing() -> NoiseConfigData {
    NoiseConfigData {
        app_static_privkey: Some([7u8; 32]),
        device_static_pubkeys: vec![vec![1, 2, 3], vec![4, 5, 6]],
    }
}

#[test]
fn test_hwi_config_network_and_pairing_do_not_clobber_each_other() {
    let tmp = TempDir::new().expect("create temp dir");
    set_datadir(tmp.path().to_path_buf());
    silent::state_crypt::unlock_for_tests();

    // No file yet: the defaults answer, and nothing is written just by reading.
    assert_eq!(
        hwi_config_network(),
        Network::Bitcoin,
        "an absent config should default to mainnet"
    );
    assert!(
        !HwiConfig::path().exists(),
        "reading the config should not create it"
    );

    let bitbox = PersistedBitboxNoiseConfig;
    assert_eq!(
        bitbox
            .read_config()
            .expect("read pairing")
            .device_static_pubkeys
            .len(),
        0,
        "an absent config should hold no pairing"
    );

    // The GUI thread writes the network.
    hwi_set_config_network(Network::Signet);
    assert_eq!(
        hwi_config_network(),
        Network::Signet,
        "network should persist"
    );
    assert!(
        HwiConfig::path().exists(),
        "writing should create the config"
    );

    // The device thread writes the pairing. The network must survive it.
    bitbox.store_config(&pairing()).expect("store pairing");
    let stored = bitbox.read_config().expect("read pairing back");
    assert_eq!(
        stored.app_static_privkey,
        Some([7u8; 32]),
        "the pairing key should persist"
    );
    assert_eq!(
        stored.device_static_pubkeys,
        vec![vec![1, 2, 3], vec![4, 5, 6]],
        "the paired devices should persist"
    );
    assert_eq!(
        hwi_config_network(),
        Network::Signet,
        "storing the pairing must not clobber the network"
    );

    // And the pairing must survive the next network write.
    hwi_set_config_network(Network::Regtest);
    assert_eq!(
        hwi_config_network(),
        Network::Regtest,
        "network should update"
    );
    assert_eq!(
        bitbox
            .read_config()
            .expect("read pairing back")
            .app_static_privkey,
        Some([7u8; 32]),
        "writing the network must not clobber the pairing"
    );

    // A corrupt file falls back to the defaults instead of failing.
    silent::state_crypt::write_state(&HwiConfig::path(), "{ not json").expect("write garbage");
    assert_eq!(
        hwi_config_network(),
        Network::Bitcoin,
        "an unparseable config should fall back to the default"
    );
    assert_eq!(
        bitbox
            .read_config()
            .expect("read pairing from garbage")
            .device_static_pubkeys
            .len(),
        0,
        "an unparseable config should hold no pairing"
    );
}
