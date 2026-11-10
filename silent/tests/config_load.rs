//! Tests for the `config_from_file` load path.
//!
//! `config_from_file` returns an opaque `Config` that reports success via `is_ok()` and
//! carries an error message via `get_error()` when loading fails, instead of fabricating a
//! usable default. These tests cover the valid, missing, and corrupt cases, plus the
//! `plugin_id` default for a valid file that omits it, and the Multikey
//! `blockchain_backend`, `birthday_time` and `birthday_height` round trips.

mod common;

use std::{
    path::PathBuf,
    sync::OnceLock,
    time::{SystemTime, UNIX_EPOCH},
};

use common::{descriptor_from_mnemonic, TEST_MNEMONIC};
use silent::{
    config_from_file, create_musig_account, save_blockchain_backend, set_datadir, Config,
    MusigCreationRequest, Network,
};
use temp_dir::TempDir;

// `set_datadir` uses a process-wide `OnceLock`, so every test in this binary must share a
// single base directory. We hold one `TempDir` for the process (kept alive so it cleans up
// on exit) and give each test a unique account name for isolation.
static DATADIR_GUARD: OnceLock<TempDir> = OnceLock::new();

/// Point `datadir()` at the shared `TempDir` and return its base path.
fn shared_datadir() -> PathBuf {
    let tmp = DATADIR_GUARD.get_or_init(|| TempDir::new().expect("create temp dir"));
    let base = tmp.path().to_path_buf();
    set_datadir(base.clone());
    silent::state_crypt::unlock_for_tests();
    base
}

/// Write a valid config file for `account_name` under the shared datadir.
fn write_valid_config(account_name: &str) {
    let config = Config::new(
        account_name.to_string(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );
    config.to_file();
    assert!(
        config_from_file(account_name.into()).is_ok(),
        "config file should exist after to_file()"
    );
}

#[test]
fn config_from_file_loads_valid_config() {
    shared_datadir();

    let account_name = "valid_account";
    write_valid_config(account_name);

    let config = config_from_file(account_name.to_string());

    assert!(
        config.is_ok(),
        "valid config should load ok, got error: {}",
        config.get_error()
    );
    assert!(
        config.get_error().is_empty(),
        "valid config should have an empty error, got {}",
        config.get_error()
    );
    assert_eq!(
        config.account_name, account_name,
        "loaded config should keep the requested account name"
    );
    assert_eq!(
        config.get_network(),
        Network::Regtest,
        "loaded network should match the saved config"
    );
}

#[test]
fn config_from_file_missing_is_not_ok() {
    shared_datadir();

    let config = config_from_file("does_not_exist_account".to_string());

    assert!(!config.is_ok(), "loading a missing config should not be ok");
    assert!(
        !config.get_error().is_empty(),
        "missing config should carry a non-empty error message"
    );
}

#[test]
fn config_from_file_corrupt_json_is_not_ok() {
    shared_datadir();

    let account_name = "corrupt_account";
    write_valid_config(account_name);
    let config = config_from_file(account_name.into());
    common::logical::write(&config, "config.json", b"{ not valid json");

    let config = config_from_file(account_name.to_string());

    assert!(!config.is_ok(), "loading corrupt JSON should not be ok");
    assert!(
        !config.get_error().is_empty(),
        "corrupt config should carry a non-empty error message"
    );
}

#[test]
fn config_from_file_defaults_plugin_id_to_sp() {
    shared_datadir();

    let account_name = "no_plugin_account";
    write_valid_config(account_name);
    let original = config_from_file(account_name.into());
    // A valid config that omits plugin_id entirely.
    let json = format!(
        r#"{{
            "account_name":"{account_name}",
            "network":"Regtest",
            "descriptor":"{}",
            "blindbit_url":"http://localhost:50001",
            "electrum_url":"",
            "dust_limit":546
        }}"#,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest)
    );
    common::logical::write(&original, "config.json", json.as_bytes());

    let config = config_from_file(account_name.to_string());

    assert!(
        config.is_ok(),
        "config without plugin_id should still load ok, got error: {}",
        config.get_error()
    );
    assert_eq!(
        config.get_plugin_id(),
        "sp",
        "plugin_id should default to \"sp\" when absent from a valid file"
    );
}

/// A full sp() descriptor carrying the spend secret, used only to exercise the
/// refusal path in `to_sp_config`. Test accounts elsewhere use the watch-only
/// `common::descriptor_from_mnemonic` instead.
fn spend_descriptor_from_mnemonic(mnemonic: &str, network: Network) -> String {
    let btc_network = match network {
        Network::Regtest => bitcoin::Network::Regtest,
        Network::Signet => bitcoin::Network::Signet,
        Network::Testnet => bitcoin::Network::Testnet,
        Network::Bitcoin => bitcoin::Network::Bitcoin,
        _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
    };
    bwk_sp::account::config::Config::new(
        "test".to_string(),
        btc_network,
        mnemonic.to_string(),
        String::new(),
        PathBuf::new(),
    )
    .unwrap()
    .descriptor
    .to_string()
}

#[test]
fn to_sp_config_refuses_a_spend_descriptor() {
    shared_datadir();

    let config = Config::new(
        "spend_descriptor_account".to_string(),
        Network::Regtest,
        spend_descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );

    let result = config.to_sp_config();
    assert!(
        matches!(result, Err(ref message) if message.contains("watch-only")),
        "expected a watch-only refusal, got {result:?}"
    );
}

#[test]
fn to_sp_config_accepts_a_watch_only_descriptor() {
    shared_datadir();

    let descriptor = descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest);
    let config = Config::new(
        "watch_only_account".to_string(),
        Network::Regtest,
        descriptor,
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );

    let result = config.to_sp_config();
    assert!(
        result.is_ok(),
        "expected a watch-only descriptor to be accepted, got {result:?}"
    );
}

// ===== Blockchain Backend Tests =====

/// The `key` entry of the stored config of `config`'s account, None when the file has none.
fn stored_entry(config: &Config, key: &str) -> Option<serde_json::Value> {
    let stored: serde_json::Value =
        serde_json::from_slice(&common::logical::read(config, "config.json"))
            .expect("the stored config is JSON");
    stored.get(key).cloned()
}

#[test]
fn blockchain_backend_round_trips_through_the_multikey_config() {
    shared_datadir();

    let account_name = "multikey_backend_account";
    let mut config = Config::new(
        account_name.to_string(),
        Network::Regtest,
        "wpkh(tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa/<0;1>/*)".to_string(),
        String::new(),
        String::new(),
        Some(546),
    );
    config.set_plugin_id("musig".to_string());
    config.to_file();
    let saved = config_from_file(account_name.to_string());
    assert!(
        saved.is_ok(),
        "the Multikey config loads: {}",
        saved.get_error()
    );
    assert_eq!(
        saved.get_blockchain_backend(),
        "",
        "a config saved with no backend reads as Electrum"
    );
    assert_eq!(
        stored_entry(&saved, "blockchain_backend"),
        None,
        "Electrum is stored as no blockchain_backend entry"
    );

    assert!(
        save_blockchain_backend(account_name.to_string(), "bitcoind".to_string()),
        "the backend choice saves"
    );
    let chosen = config_from_file(account_name.to_string());
    assert!(chosen.is_ok(), "the config loads: {}", chosen.get_error());
    assert_eq!(
        chosen.get_blockchain_backend(),
        "bitcoind",
        "the chosen module id reads back"
    );
    assert_eq!(
        stored_entry(&chosen, "blockchain_backend"),
        Some(serde_json::Value::from("bitcoind")),
        "the module id is stored as blockchain_backend"
    );
    assert_eq!(
        chosen.get_plugin_id(),
        "musig",
        "saving the backend keeps the account a Multikey one"
    );

    assert!(
        save_blockchain_backend(account_name.to_string(), String::new()),
        "Electrum saves back"
    );
    let electrum = config_from_file(account_name.to_string());
    assert_eq!(
        electrum.get_blockchain_backend(),
        "",
        "an empty module id reads back as Electrum"
    );
    assert_eq!(
        stored_entry(&electrum, "blockchain_backend"),
        None,
        "Electrum saved back leaves no blockchain_backend entry"
    );
}

#[test]
fn blockchain_backend_of_a_missing_account_does_not_save() {
    shared_datadir();

    assert!(
        !save_blockchain_backend("no_such_account".to_string(), "bitcoind".to_string()),
        "a missing account saves no backend"
    );
}

// ===== Birthday Tests =====

/// Unix seconds now.
fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("the clock is past 1970")
        .as_secs()
}

#[test]
fn a_created_multikey_account_records_its_birthday() {
    shared_datadir();

    let secp = bitcoin::secp256k1::Secp256k1::new();
    let master = bitcoin::bip32::Xpriv::new_master(bitcoin::Network::Regtest, &[23; 32])
        .expect("test master");
    let account_name = "multikey_birthday_account";
    let before = now();
    let created = create_musig_account(MusigCreationRequest {
        account_name: account_name.into(),
        network: Network::Regtest,
        descriptor: format!(
            "tr([{}]{}/<0;1>/*)",
            master.fingerprint(&secp),
            bitcoin::bip32::Xpub::from_priv(&secp, &master)
        ),
        electrum_url: String::new(),
        verify_certificate: true,
    });
    assert!(created.is_ok, "create Multikey: {}", created.error);
    let after = now();

    let saved = config_from_file(account_name.to_string());
    assert!(saved.is_ok(), "the config loads: {}", saved.get_error());
    let birthday = saved.get_birthday_time();
    assert!(
        (before..=after).contains(&birthday),
        "the birthday {birthday} is the creation time, between {before} and {after}"
    );
    assert_eq!(
        stored_entry(&saved, "birthday_time"),
        Some(serde_json::Value::from(birthday)),
        "the birthday is stored as birthday_time"
    );
}

#[test]
fn a_multikey_config_without_a_birthday_loads_with_none() {
    shared_datadir();

    // A config written before the birthday was recorded has no birthday_time entry.
    let account_name = "multikey_no_birthday_account";
    let mut config = Config::new(
        account_name.to_string(),
        Network::Regtest,
        "wpkh(tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa/<0;1>/*)".to_string(),
        String::new(),
        String::new(),
        Some(546),
    );
    config.set_plugin_id("musig".to_string());
    config.to_file();
    assert_eq!(
        stored_entry(&config, "birthday_time"),
        None,
        "no birthday is stored as no birthday_time entry"
    );
    let saved = config_from_file(account_name.to_string());
    assert!(
        saved.is_ok(),
        "a config without a birthday loads: {}",
        saved.get_error()
    );
    assert_eq!(saved.get_birthday_time(), 0, "it reads as no birthday");
    assert!(
        save_blockchain_backend(account_name.to_string(), "bitcoind".to_string()),
        "the backend choice saves"
    );
    let rewritten = config_from_file(account_name.to_string());
    assert_eq!(
        (
            rewritten.get_birthday_time(),
            stored_entry(&rewritten, "birthday_time")
        ),
        (0, None),
        "writing the config again records no birthday"
    );
}

#[test]
fn a_multikey_birthday_height_round_trips_through_the_config() {
    shared_datadir();

    let account_name = "multikey_birthday_height_account";
    let mut config = Config::new(
        account_name.to_string(),
        Network::Regtest,
        "wpkh(tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa/<0;1>/*)".to_string(),
        String::new(),
        String::new(),
        Some(546),
    );
    config.set_plugin_id("musig".to_string());
    config.to_file();
    assert_eq!(
        (
            config.get_birthday_height(),
            stored_entry(&config, "birthday_height")
        ),
        (0, None),
        "no birthday height is stored as no birthday_height entry and reads as 0"
    );
    config.birthday_height = Some(150);
    config.to_file();
    let saved = config_from_file(account_name.to_string());
    assert!(saved.is_ok(), "the config loads: {}", saved.get_error());
    assert_eq!(
        (
            saved.get_birthday_height(),
            stored_entry(&saved, "birthday_height")
        ),
        (150, Some(serde_json::Value::from(150))),
        "the birthday height is stored as birthday_height and reads back"
    );
    assert!(
        save_blockchain_backend(account_name.to_string(), "bitcoind".to_string()),
        "the backend choice saves"
    );
    assert_eq!(
        config_from_file(account_name.to_string()).get_birthday_height(),
        150,
        "writing the config again keeps it"
    );
}
