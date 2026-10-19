//! The account config's Electrum certificate check: its default, its round trip, the
//! reset on another server, and the policy both bwk account configs connect under.

mod common;

use std::path::PathBuf;
use std::str::FromStr;
use std::sync::OnceLock;

use bitcoin::bip32::{DerivationPath, Xpriv, Xpub};
use bitcoin::secp256k1::Secp256k1;
use bwk_sp::bwk::bwk_electrum::raw_client::CertificateCheck;
use common::{descriptor_from_mnemonic, TEST_MNEMONIC};
use silent::{config_from_file, new_config, set_datadir, Config, Network, MUSIG_PLUGIN_ID};
use temp_dir::TempDir;

// `set_datadir` is process-wide: every test of this binary shares one base directory
// and uses its own account name.
static DATADIR_GUARD: OnceLock<TempDir> = OnceLock::new();

fn shared_datadir() -> PathBuf {
    let tmp = DATADIR_GUARD.get_or_init(|| TempDir::new().expect("create temp dir"));
    let base = tmp.path().to_path_buf();
    set_datadir(base.clone());
    silent::state_crypt::unlock_for_tests();
    base
}

const ELECTRUM_URL: &str = "ssl://electrum.example:50002";

fn sp_config(account_name: &str) -> Config {
    Config::new(
        account_name.to_string(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:8080".to_string(),
        ELECTRUM_URL.to_string(),
        Some(546),
    )
}

/// `tr(musig(A,B)/<0;1>/*)` over the keys of seeds 1 and 2, at their Cosigner role.
fn musig_descriptor() -> String {
    let secp = Secp256k1::new();
    let keys: Vec<String> = [1u8, 2]
        .iter()
        .map(|seed| {
            let master = Xpriv::new_master(bitcoin::Network::Regtest, &[*seed; 32]).unwrap();
            let role = master
                .derive_priv(&secp, &DerivationPath::from_str("m/87'/1'/0'/2").unwrap())
                .unwrap();
            format!(
                "[{}/87'/1'/0'/2]{}",
                master.fingerprint(&secp),
                Xpub::from_priv(&secp, &role)
            )
        })
        .collect();
    format!("tr(musig({},{})/<0;1>/*)", keys[0], keys[1])
}

#[test]
fn a_config_written_before_the_setting_verifies_the_certificate() {
    let base = shared_datadir();
    let account_name = "cert_old_account";
    let account_dir = base.join(account_name);
    std::fs::create_dir_all(&account_dir).expect("create account dir");
    let json = format!(
        r#"{{
            "account_name":"{account_name}",
            "network":"Regtest",
            "descriptor":"{}",
            "blindbit_url":"http://localhost:8080",
            "electrum_url":"{ELECTRUM_URL}",
            "dust_limit":546
        }}"#,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest)
    );
    silent::state_crypt::write_state(&account_dir.join("config.json"), &json)
        .expect("write old config");

    let config = config_from_file(account_name.to_string());

    assert!(
        config.is_ok(),
        "old config should load, got: {}",
        config.get_error()
    );
    assert!(
        config.get_verify_certificate(),
        "a config without the setting should verify the certificate"
    );
}

#[test]
fn a_skipped_certificate_check_round_trips() {
    shared_datadir();
    let account_name = "cert_skip_account";
    let mut config = sp_config(account_name);
    config.set_verify_certificate(false);
    config.to_file();

    let loaded = config_from_file(account_name.to_string());

    assert!(
        loaded.is_ok(),
        "config should load, got: {}",
        loaded.get_error()
    );
    assert!(
        !loaded.get_verify_certificate(),
        "a skipped certificate check should be read back skipped"
    );
}

#[test]
fn another_server_verifies_the_certificate_again() {
    let mut config = sp_config("cert_reset_account");
    config.set_verify_certificate(false);

    config.set_electrum_url(ELECTRUM_URL.to_string());
    assert!(
        !config.get_verify_certificate(),
        "the same server should keep the skipped check"
    );

    config.set_electrum_url("ssl://other.example:50002".to_string());
    assert!(
        config.get_verify_certificate(),
        "another server should check its certificate again"
    );
}

#[test]
fn the_sp_account_connects_under_the_configured_check() {
    shared_datadir();
    for (verify, expected) in [
        (true, CertificateCheck::Validate),
        (false, CertificateCheck::DangerAcceptInvalid),
    ] {
        let mut config = sp_config("cert_sp_account");
        config.set_verify_certificate(verify);

        let sp = config.to_sp_config().expect("sp config");

        assert_eq!(
            sp.endpoint().certificate_check(),
            expected,
            "verify_certificate {verify}: the account's endpoint"
        );
    }
}

#[test]
fn the_musig_account_connects_under_the_configured_check() {
    shared_datadir();
    for (verify, expected) in [
        (true, CertificateCheck::Validate),
        (false, CertificateCheck::DangerAcceptInvalid),
    ] {
        let mut config = new_config(
            "cert_musig_account".to_string(),
            Network::Regtest,
            musig_descriptor(),
            String::new(),
            ELECTRUM_URL.to_string(),
            0,
            MUSIG_PLUGIN_ID.to_string(),
        );
        config.set_verify_certificate(verify);

        let musig = config.to_musig_config().expect("musig config");

        assert_eq!(
            musig.scanner.endpoint().certificate_check(),
            expected,
            "verify_certificate {verify}: the scanner's endpoint"
        );
    }
}
