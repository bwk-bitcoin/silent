//! MusigAccount binding tests: config round trip, descriptor validation, Regtest
//! account, addresses and spend paths.

mod common;

use std::str::FromStr;

use bitcoin::bip32::{DerivationPath, Fingerprint, Xpriv, Xpub};
use bitcoin::secp256k1::Secp256k1;
use silent::{
    config_from_file, list_configs, musig_descriptor_fingerprints, new_config, new_musig_account,
    validate_musig_descriptor, Config, Network, MUSIG_PLUGIN_ID,
};

use common::{cleanup_test_account, descriptor_from_mnemonic, test_account_name, TEST_MNEMONIC};

// ===== Test Helpers =====

/// The master fingerprint of the key from `[seed; 32]` and its Cosigner role key
/// `[fp/87'/1'/0'/2]xpub`, the xpub at the unhardened child 2 of its BIP-87 account.
fn key(seed: u8) -> (Fingerprint, String) {
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).unwrap();
    let role = master
        .derive_priv(&secp, &DerivationPath::from_str("m/87'/1'/0'/2").unwrap())
        .unwrap();
    let fingerprint = master.fingerprint(&secp);
    let xpub = Xpub::from_priv(&secp, &role);
    (fingerprint, format!("[{fingerprint}/87'/1'/0'/2]{xpub}"))
}

/// `tr(musig(A,B)/<0;1>/*)` over seeds 1 and 2.
fn two_of_two() -> String {
    let (_, a) = key(1);
    let (_, b) = key(2);
    format!("tr(musig({a},{b})/<0;1>/*)")
}

/// `tr(musig(A,B)/<0;1>/*,pk(C/<2;3>/*))` over seeds 1, 2 and 3.
fn with_leaf() -> String {
    let (_, a) = key(1);
    let (_, b) = key(2);
    let (_, c) = key(3);
    format!("tr(musig({a},{b})/<0;1>/*,pk({c}/<2;3>/*))")
}

/// Write a Regtest musig account config named `account_name` over `descriptor`.
fn write_musig_config(account_name: &str, descriptor: String) -> Box<Config> {
    let config = new_config(
        account_name.to_string(),
        Network::Regtest,
        descriptor,
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.to_string(),
    );
    config.to_file();
    config
}

fn fingerprints_of(seeds: &[u8]) -> Vec<String> {
    let mut fingerprints: Vec<String> = seeds.iter().map(|s| key(*s).0.to_string()).collect();
    fingerprints.sort();
    fingerprints
}

// ===== Config Tests =====

#[test]
fn musig_config_round_trip() {
    let account_name = test_account_name();
    write_musig_config(&account_name, two_of_two());

    let loaded = config_from_file(account_name.clone());
    assert!(loaded.is_ok(), "config should load: {}", loaded.get_error());
    assert_eq!(
        loaded.get_plugin_id(),
        "musig",
        "plugin id should be musig, got {}",
        loaded.get_plugin_id()
    );
    assert_eq!(
        loaded.get_descriptor(),
        two_of_two(),
        "descriptor should round trip, got {}",
        loaded.get_descriptor()
    );
    assert!(
        matches!(loaded.get_network(), Network::Regtest),
        "network should be Regtest, got {:?}",
        loaded.get_network()
    );
    assert!(
        loaded.get_blindbit_url().is_empty(),
        "blindbit url should be empty, got {}",
        loaded.get_blindbit_url()
    );
    let configs = list_configs();
    assert!(
        configs.contains(&account_name),
        "list_configs should contain {account_name}, got {configs:?}"
    );

    cleanup_test_account(&account_name);
}

#[test]
fn musig_config_to_bwk_config() {
    let account_name = test_account_name();
    let config = write_musig_config(&account_name, two_of_two());

    let musig_config = config.to_musig_config();
    assert!(
        musig_config.is_ok(),
        "to_musig_config should succeed, got {:?}",
        musig_config.as_ref().err()
    );
    let musig_config = musig_config.unwrap();
    assert_eq!(
        musig_config.scanner.account_dir(),
        config.account_dir(),
        "bwk stores should sit in the silent account directory"
    );

    cleanup_test_account(&account_name);
}

#[test]
fn musig_config_refuses_other_plugin() {
    let account_name = test_account_name();
    let config = new_config(
        account_name.clone(),
        Network::Regtest,
        two_of_two(),
        String::new(),
        String::new(),
        0,
        "sp".to_string(),
    );
    config.to_file();

    let error = config.to_musig_config().err();
    assert!(
        error.as_ref().is_some_and(|e| e.contains("'sp'")),
        "to_musig_config should refuse plugin id sp naming it, got {error:?}"
    );
    let account = new_musig_account(account_name.clone());
    assert!(!account.is_ok(), "an sp config should not open as musig");
    assert!(
        !account.get_error().is_empty(),
        "the refusal should carry an error"
    );

    cleanup_test_account(&account_name);
}

// ===== Descriptor Tests =====

#[test]
fn validate_musig_descriptor_canonical() {
    let result = validate_musig_descriptor(two_of_two());
    assert!(result.is_ok, "2-of-2 should validate: {}", result.error);
    assert!(
        result.descriptor.contains('#'),
        "canonical text should carry a checksum, got {}",
        result.descriptor
    );
    let again = validate_musig_descriptor(result.descriptor.clone());
    assert!(
        again.is_ok,
        "canonical text should validate: {}",
        again.error
    );
    assert_eq!(
        again.descriptor, result.descriptor,
        "canonical text should be stable"
    );

    let (_, a) = key(1);
    let (_, b) = key(2);
    for refused in [
        "not a descriptor".to_string(),
        format!("wsh(pk(musig({a},{b})/<0;1>/*))"),
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
    ] {
        let result = validate_musig_descriptor(refused.clone());
        assert!(!result.is_ok, "{refused} should be refused");
        assert!(
            result.descriptor.is_empty(),
            "a refused descriptor should give no text, got {}",
            result.descriptor
        );
        assert!(!result.error.is_empty(), "{refused} should carry an error");
    }
}

#[test]
fn musig_descriptor_fingerprints_lists_every_key() {
    let two = musig_descriptor_fingerprints(two_of_two());
    assert_eq!(
        two,
        fingerprints_of(&[1, 2]),
        "2-of-2 should list both keys"
    );
    let three = musig_descriptor_fingerprints(with_leaf());
    assert_eq!(
        three,
        fingerprints_of(&[1, 2, 3]),
        "with_leaf should list three keys"
    );
    let none = musig_descriptor_fingerprints("not a descriptor".to_string());
    assert!(none.is_empty(), "garbage should list nothing, got {none:?}");
}

// ===== Account Tests =====

#[test]
fn new_musig_account_on_regtest() {
    let account_name = test_account_name();
    write_musig_config(&account_name, two_of_two());

    let account = new_musig_account(account_name.clone());
    assert!(
        account.is_ok(),
        "account should open: {}",
        account.get_error()
    );
    assert_eq!(account.name(), account_name, "account name");
    assert!(
        matches!(account.network(), Network::Regtest),
        "network should be Regtest, got {:?}",
        account.network()
    );
    let canonical = validate_musig_descriptor(two_of_two());
    assert_eq!(
        validate_musig_descriptor(account.descriptor()).descriptor,
        canonical.descriptor,
        "account descriptor should be the canonical 2-of-2"
    );
    let mut cosigners = account.cosigner_fingerprints();
    cosigners.sort();
    assert_eq!(cosigners, fingerprints_of(&[1, 2]), "both cosigners");
    assert_eq!(account.balance(), 0, "a fresh account has no balance");
    assert!(account.coins().is_empty(), "a fresh account has no coins");
    assert!(
        account.electrum_url().is_empty(),
        "no electrum url was set, got {}",
        account.electrum_url()
    );
    drop(account);

    let config = Config::from_file(account_name.clone());
    assert!(
        config
            .as_ref()
            .is_ok_and(|c| c.get_plugin_id() == MUSIG_PLUGIN_ID),
        "config.json should still be silent's musig config, got {config:?}"
    );

    cleanup_test_account(&account_name);
}

#[test]
fn musig_account_addresses() {
    let account_name = test_account_name();
    write_musig_config(&account_name, two_of_two());
    let mut account = new_musig_account(account_name.clone());
    assert!(
        account.is_ok(),
        "account should open: {}",
        account.get_error()
    );

    let first = account.new_addr();
    let second = account.new_addr();
    assert!(
        first.starts_with("bcrt1p"),
        "address should be regtest taproot, got {first}"
    );
    assert!(
        second.starts_with("bcrt1p"),
        "address should be regtest taproot, got {second}"
    );
    assert_ne!(first, second, "each new address should differ");

    let info = account.address_verify_info(&first);
    assert_eq!(info.descriptor, account.descriptor(), "verify descriptor");
    // bwk hands out receive index 1 first (the tip starts at 0 and moves before use).
    assert_eq!(info.path, "0/1", "first receive address path");

    let unknown = account.address_verify_info("bcrt1qunknown");
    assert!(
        unknown.descriptor.is_empty() && unknown.path.is_empty(),
        "an unknown address gives empty fields, got {} {}",
        unknown.descriptor,
        unknown.path
    );

    drop(account);
    cleanup_test_account(&account_name);
}

#[test]
fn musig_spend_leaves_key_path_first() {
    let account_name = test_account_name();
    write_musig_config(&account_name, with_leaf());
    let account = new_musig_account(account_name.clone());
    assert!(
        account.is_ok(),
        "account should open: {}",
        account.get_error()
    );

    let result = account.spend_leaves(vec![]);
    assert!(
        result.is_ok,
        "spend_leaves should succeed: {}",
        result.error
    );
    assert_eq!(result.leaves.len(), 2, "key path and one leaf");

    let key_path = &result.leaves[0];
    assert!(key_path.key_path, "the key path comes first");
    assert!(
        key_path.leaf_hash.is_empty(),
        "the key path has no leaf hash, got {}",
        key_path.leaf_hash
    );
    let mut signers = key_path.fingerprints.clone();
    signers.sort();
    assert_eq!(signers, fingerprints_of(&[1, 2]), "key path signers");

    let leaf = &result.leaves[1];
    assert!(!leaf.key_path, "the second entry is a leaf");
    assert_eq!(leaf.fingerprints, fingerprints_of(&[3]), "leaf signer");
    assert!(
        leaf.leaf_hash.len() == 64 && leaf.leaf_hash.chars().all(|c| c.is_ascii_hexdigit()),
        "leaf hash should be 64 hex chars, got {}",
        leaf.leaf_hash
    );

    drop(account);
    cleanup_test_account(&account_name);
}

#[test]
fn missing_musig_account() {
    let account = new_musig_account(format!("does_not_exist_{}", std::process::id()));
    assert!(!account.is_ok(), "a missing account should not open");
    assert!(
        !account.get_error().is_empty(),
        "a missing account should carry an error"
    );
}
