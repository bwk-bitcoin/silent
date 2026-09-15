//! Tests for the `config_from_file` load path.
//!
//! `config_from_file` returns an opaque `Config` that reports success via `is_ok()` and
//! carries an error message via `get_error()` when loading fails, instead of fabricating a
//! usable default. These tests cover the valid, missing, and corrupt cases, plus the
//! `plugin_id` default for a valid file that omits it.

mod common;

use std::path::PathBuf;
use std::sync::OnceLock;

use common::{descriptor_from_mnemonic, TEST_MNEMONIC};
use silent::{config_from_file, set_datadir, Config, Network};
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
        config.config_path().exists(),
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
    let base = shared_datadir();

    let account_name = "corrupt_account";
    // The config lives at <datadir>/<account_name>/config.json.
    let account_dir = base.join(account_name);
    std::fs::create_dir_all(&account_dir).expect("create account dir");
    std::fs::write(account_dir.join("config.json"), "{ not valid json")
        .expect("write corrupt config");

    let config = config_from_file(account_name.to_string());

    assert!(!config.is_ok(), "loading corrupt JSON should not be ok");
    assert!(
        !config.get_error().is_empty(),
        "corrupt config should carry a non-empty error message"
    );
}

#[test]
fn config_from_file_defaults_plugin_id_to_sp() {
    let base = shared_datadir();

    let account_name = "no_plugin_account";
    let account_dir = base.join(account_name);
    std::fs::create_dir_all(&account_dir).expect("create account dir");
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
    std::fs::write(account_dir.join("config.json"), json).expect("write config without plugin_id");

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
