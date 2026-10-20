//! Real SP preparation through the shared Send and SpSigning widgets, without devices.

#![cfg(target_os = "linux")]

mod common;

use std::{env, os::unix::fs::symlink, process::Command};

use bitcoin::{
    bip32::{ChainCode, ChildNumber, Xpub},
    secp256k1::{PublicKey, Secp256k1, SecretKey},
    NetworkKind,
};
use bwk_utils::test as bwk_test;
use silent::{config::datadir, state_create_with_device, Account, Config, Network};
use temp_dir::TempDir;

use common::{
    descriptor_from_mnemonic, fund_sp_wallet, isolated_state, setup_blindbitd, test_account_name,
    wait_for_scan_complete, wait_for_sync_and_index, TEST_MNEMONIC,
};

#[test]
#[ignore = "requires built host_smoke (SILENT_HOST_SMOKE), bitcoind and BlindbitD"]
fn prepared_sp_send_is_locked_during_validation_and_signing() {
    let probe = env::var_os("SILENT_HOST_SMOKE").expect("path to the built host_smoke");
    isolated_state();
    let xpub = Xpub {
        network: NetworkKind::Test,
        depth: 0,
        parent_fingerprint: Default::default(),
        child_number: ChildNumber::from_normal_idx(0).expect("master child number"),
        public_key: PublicKey::from_secret_key(
            &Secp256k1::new(),
            &SecretKey::from_slice(&[7; 32]).expect("shared public test key"),
        ),
        chain_code: ChainCode::from([0; 32]),
    }
    .to_string();
    let error = state_create_with_device(0, xpub.clone());
    assert!(
        error.is_empty(),
        "write unlock metadata for the shared test key: {error}"
    );
    let (bbd, mut node) = setup_blindbitd();
    bwk_test::generate_blocks(&mut node.client, 101);
    let url = bbd.url();
    wait_for_sync_and_index(&url, 101);
    let name = test_account_name();
    let config = Config::new(
        name.clone(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        url.clone(),
        String::new(),
        Some(546),
    );
    config.to_file();
    let mut account = Account::new(config).expect("create real SP fixture account");
    assert!(account.start_scanner(), "start real SP scanner");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "finish initial SP scan"
    );
    fund_sp_wallet(&mut node.client, &url, TEST_MNEMONIC, 0.1);
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "scan funded SP transaction"
    );
    assert!(
        account.balance() > 0,
        "fixture must contain actual spendable SP coins"
    );
    account.stop_scanner();
    drop(account);
    let home = TempDir::new().expect("GUI fixture home directory");
    symlink(datadir(), home.path().join(".silent")).expect("share the isolated funded state");
    let output = Command::new(probe)
        .arg("--sp-signing")
        .arg(xpub)
        .arg(name)
        .env("HOME", home.path())
        .env("QT_QPA_PLATFORM", "offscreen")
        .output()
        .expect("launch the real Send/SP signing probe");
    assert!(
        output.status.success(),
        "SP preparation GUI regression failed:\n{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        String::from_utf8_lossy(&output.stdout).contains("SP signing smoke: OK"),
        "probe must exercise the SP preparation/signing branch"
    );
}
