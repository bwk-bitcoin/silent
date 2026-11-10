//! Tests for the `config_from_file` load path.
//!
//! `config_from_file` returns an opaque `Config` that reports success via `is_ok()` and
//! carries an error message via `get_error()` when loading fails, instead of fabricating a
//! usable default. These tests cover the valid, missing, and corrupt cases, plus the
//! `plugin_id` default for a valid file that omits it, and the Multikey
//! `blockchain_backend` round trip.

mod common;

use std::path::PathBuf;
use std::sync::OnceLock;

use common::{descriptor_from_mnemonic, TEST_MNEMONIC};
use silent::{config_from_file, save_blockchain_backend, set_datadir, Config, Network};
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

/// The `blockchain_backend` entry of the stored config of `config`'s account, None when
/// the file has none.
fn stored_blockchain_backend(config: &Config) -> Option<serde_json::Value> {
    let stored: serde_json::Value =
        serde_json::from_slice(&common::logical::read(config, "config.json"))
            .expect("the stored config is JSON");
    stored.get("blockchain_backend").cloned()
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
        stored_blockchain_backend(&saved),
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
        stored_blockchain_backend(&chosen),
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
        stored_blockchain_backend(&electrum),
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
