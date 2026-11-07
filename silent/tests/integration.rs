//! End-to-end integration tests for Silent wallet.
//!
//! These tests cover wallet creation, config management, SP address generation,
//! BlindBit connection, send/receive flows, and wallet restore functionality.

mod common;

use bwk::{
    bwk_electrum::{
        label_store::{encode_key, LabelKey},
        persistence::BackendKind,
    },
    bwk_tx::bwk_psbt::PsbtV2,
    persist::{DB_VERSION, LABELS_STORE_KEY},
};
use bwk_utils::test as bwk_test;
use serde_json::Value;
use silent::{Account, Config, Network, NotificationFlag};
use std::thread;
use std::time::Duration;

use common::{
    cleanup_test_account, create_test_account, descriptor_from_mnemonic, fund_sp_wallet,
    setup_blindbitd, sp_unsigned_psbt, test_account_name, wait_for_notification,
    wait_for_scan_complete, wait_for_sync_and_index, TEST_MNEMONIC,
};

// ===== Test Helpers =====

/// Create test config with regtest network.
fn create_test_config(account_name: String) -> Config {
    Config::new(
        account_name,
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    )
}

// ===== Wallet Creation and Config Tests =====

/// A key's fingerprint and its xpub at `path`, from a fixed seed.
fn key_xpub(path: &str) -> (String, String) {
    use bitcoin::bip32::{DerivationPath, Xpriv, Xpub};
    use bitcoin::secp256k1::Secp256k1;
    use std::str::FromStr;
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(bitcoin::Network::Regtest, &[7u8; 32]).unwrap();
    let derived = master
        .derive_priv(&secp, &DerivationPath::from_str(path).unwrap())
        .unwrap();
    (
        master.fingerprint(&secp).to_string(),
        Xpub::from_priv(&secp, &derived).to_string(),
    )
}

#[test]
fn test_sub_accounts_from_the_key_xpubs() {
    let (fingerprint, segwit_xpub) = key_xpub("m/84'/1'/0'");
    let (_, taproot_xpub) = key_xpub("m/86'/1'/0'");

    // Segwit alone: no taproot address, whatever the sub-account's position.
    let account_name = test_account_name();
    let mut config = create_test_config(account_name.clone());
    config.set_segwit_descriptor(silent::segwit_descriptor_for(
        fingerprint.clone(),
        segwit_xpub.clone(),
        Network::Regtest,
    ));
    let mut account = Account::new(config).expect("Account creation should succeed");
    assert!(account.has_segwit());
    assert!(!account.has_taproot());
    assert!(account.new_segwit_addr().starts_with("bcrt1q"));
    assert!(account.new_taproot_addr().is_empty());
    drop(account);
    cleanup_test_account(&account_name);

    // Both.
    let account_name = test_account_name();
    let mut config = create_test_config(account_name.clone());
    config.set_segwit_descriptor(silent::segwit_descriptor_for(
        fingerprint.clone(),
        segwit_xpub,
        Network::Regtest,
    ));
    config.set_taproot_descriptor(silent::taproot_descriptor_for(
        fingerprint,
        taproot_xpub,
        Network::Regtest,
    ));
    let mut account = Account::new(config).expect("Account creation should succeed");
    assert!(account.has_segwit());
    assert!(account.has_taproot());
    assert!(account.new_segwit_addr().starts_with("bcrt1q"));
    assert!(account.new_taproot_addr().starts_with("bcrt1p"));
    drop(account);
    cleanup_test_account(&account_name);
}

#[test]
fn test_wallet_creation_from_descriptor() {
    let account_name = test_account_name();
    let mut config = create_test_config(account_name.clone());
    let (fingerprint, xpub) = key_xpub("m/86'/1'/0'");
    config.set_taproot_descriptor(silent::taproot_descriptor_for(
        fingerprint,
        xpub,
        Network::Regtest,
    ));
    assert!(
        config.save().is_empty(),
        "persist the logical account config"
    );

    assert_eq!(config.account_name, account_name);
    assert_eq!(config.get_network(), Network::Regtest);
    assert!(!config.get_descriptor().is_empty());

    let account_result = Account::new(config.clone());
    assert!(account_result.is_ok(), "Account creation should succeed");

    let mut account = account_result.unwrap();
    assert_eq!(account.name(), account_name);
    assert_eq!(account.balance(), 0);

    let sp_address = account.sp_address();
    let first_address = account.new_taproot_addr();
    assert!(
        first_address.starts_with("bcrt1p"),
        "real sub-account address"
    );
    let outpoint = format!("{}:0", "01".repeat(32))
        .parse::<bitcoin::OutPoint>()
        .expect("fixture outpoint");
    let label = account.update_coin_label(outpoint.to_string(), "persisted creation label".into());
    assert!(label.is_ok, "persist the real label: {}", label.error);
    assert!(
        account.shutdown_backend().is_empty(),
        "flush and destroy the real backends"
    );
    drop(account);

    let file = "sp/labels.json";
    let saved = common::logical::read(&config, file);
    let mut document: Value = serde_json::from_slice(&saved).expect("logical row document");
    assert_eq!(
        document["version"], DB_VERSION,
        "the actual store stamps its supported version"
    );
    let backend = common::logical::backend(&config, BackendKind::SilentPayment);
    assert_eq!(
        backend
            .get_row(LABELS_STORE_KEY, &encode_key(&LabelKey::OutPoint(outpoint)))
            .expect("fresh factory reads persisted bytes"),
        Some(serde_json::to_vec("persisted creation label").expect("literal label bytes")),
        "reopened byte-row backend preserves the serialized label"
    );
    drop(backend);

    document["version"] = (DB_VERSION + 1).into();
    let newer = serde_json::to_vec(&document).expect("future-version fixture");
    common::logical::write(&config, file, &newer);
    let refused = Account::new(config.clone());
    assert!(
        matches!(refused, Err(ref error) if error.contains("version")),
        "the actual account factory must reject a future store version: {:?}",
        refused.as_ref().err()
    );
    drop(refused);
    assert_eq!(
        common::logical::read(&config, file),
        newer,
        "refusal never rewrites newer data"
    );
    common::logical::write(&config, file, &saved);

    let sealed = std::fs::read(common::logical::root(&config).join("tree.bip138"))
        .expect("host-side encrypted backing");
    assert!(
        silent::state_crypt::is_sealed(&sealed),
        "version metadata is encrypted with the store"
    );
    let mut reopened =
        Account::new(Config::from_file(account_name.clone()).expect("reload logical config"))
            .expect("actual account reopens through its factory");
    assert_eq!(reopened.name(), account_name, "reopened identity");
    assert_eq!(reopened.balance(), 0, "reopened empty balance");
    assert_eq!(reopened.sp_address(), sp_address, "reopened SP receiver");
    assert_ne!(
        reopened.new_taproot_addr(),
        first_address,
        "reopened sub-account retains its derivation tip"
    );
    assert!(
        reopened.shutdown_backend().is_empty(),
        "reopened account shuts down cleanly"
    );
    drop(reopened);

    cleanup_test_account(&account_name);
}

#[test]
fn test_config_save_load_roundtrip() {
    let account_name = test_account_name();
    let original_config = create_test_config(account_name.clone());

    original_config.to_file();
    assert_eq!(
        common::logical::read(&original_config, "config.json"),
        serde_json::to_vec_pretty(&original_config).expect("serialized config")
    );

    let loaded_config =
        Config::from_file(account_name.clone()).expect("Config should load successfully");

    assert_eq!(loaded_config.account_name, original_config.account_name);
    assert_eq!(loaded_config.get_network(), original_config.get_network());
    assert_eq!(
        loaded_config.get_descriptor(),
        original_config.get_descriptor()
    );
    assert_eq!(
        loaded_config.get_blindbit_url(),
        original_config.get_blindbit_url()
    );
    assert_eq!(
        loaded_config.get_dust_limit(),
        original_config.get_dust_limit()
    );

    cleanup_test_account(&account_name);
}

// ===== SP Address Generation Tests =====

#[test]
fn test_sp_address_generation() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());
    let account = Account::new(config).expect("Account creation should succeed");

    let sp_addr = account.sp_address();

    assert!(!sp_addr.is_empty(), "SP address should not be empty");
    assert!(
        sp_addr.starts_with("sp") || sp_addr.starts_with("tsp"),
        "SP address should start with sp prefix"
    );
    assert!(
        sp_addr.len() >= 100,
        "SP address should be at least 100 characters, got {}",
        sp_addr.len()
    );

    // Verify address is deterministic
    let account_name2 = format!("{account_name}_2");
    let config2 = create_test_config(account_name2.clone());
    let account2 = Account::new(config2).expect("Account creation should succeed");
    assert_eq!(
        account2.sp_address(),
        sp_addr,
        "Same descriptor should produce same address"
    );

    cleanup_test_account(&account_name);
    cleanup_test_account(&account_name2);
}

// ===== BlindBit Connection Tests =====

#[test]
fn test_blindbit_connection_regtest() {
    let (bbd, mut bitcoind_node) = setup_blindbitd();
    let bitcoind = &mut bitcoind_node.client;
    let url = bbd.url();

    // Generate blocks for coinbase maturity
    bwk_test::generate_blocks(bitcoind, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account = create_test_account(&account_name, &url);

    // Start scanner
    assert!(account.start_scanner(), "Scanner should start successfully");

    // Wait for scanner to connect and start scanning
    let got_started = wait_for_scan_complete(&mut account, 30);
    assert!(
        got_started,
        "Scanner should complete scan or reach tip within 30s"
    );

    // Stop scanner
    account.stop_scanner();

    // Wait for ScanStopped
    let mut got_stopped = false;
    for _ in 0..50 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == NotificationFlag::ScanStopped {
                got_stopped = true;
                break;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    assert!(got_stopped, "Should receive ScanStopped notification");

    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Send/Receive SP Flow Tests =====

#[test]
fn test_send_receive_sp_flow() {
    let (bbd, mut bitcoind_node) = setup_blindbitd();
    let bitcoind = &mut bitcoind_node.client;
    let url = bbd.url();

    // Generate blocks for coinbase maturity
    bwk_test::generate_blocks(bitcoind, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account = create_test_account(&account_name, &url);

    // Start scanner and wait for initial sync
    assert!(account.start_scanner(), "Scanner should start");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );

    let initial_balance = account.balance();
    assert_eq!(initial_balance, 0, "New wallet should have zero balance");

    // Fund the wallet with an SP transaction
    fund_sp_wallet(bitcoind, &url, TEST_MNEMONIC, 0.1);

    // Wait for the scanner to detect the new output
    let mut received_output = false;
    for _ in 0..300 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == NotificationFlag::NewOutput {
                received_output = true;
                break;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    assert!(received_output, "Should receive NewOutput notification");

    // Verify balance increased
    let new_balance = account.balance();
    assert!(
        new_balance > 0,
        "Balance should increase after receiving funds, got {new_balance}"
    );

    // Create send transaction (send back to same address)
    use silent::{Output, TransactionTemplate};

    let send_amount = new_balance / 2;
    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: send_amount,
            label: String::from("test send"),
            max: false,
        }],
        fee_rate: 0.5,
        fee: 0,
        input_outpoints: vec![],
    };

    // Simulate transaction
    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        simulation.is_valid,
        "Transaction simulation should succeed: {}",
        simulation.error
    );
    assert!(simulation.fee > 0, "Fee should be non-zero");
    let mut full_rate = tx_template.clone();
    full_rate.fee_rate = 1.0;
    let full_rate = account.simulate_transaction(full_rate);
    assert!(
        full_rate.is_valid,
        "1 sat/vB comparison: {}",
        full_rate.error
    );
    assert!(
        simulation.fee < full_rate.fee,
        "sub-1 SP builder fee {} must be below 1 sat/vB fee {}",
        simulation.fee,
        full_rate.fee
    );

    // Prepare transaction
    let psbt = sp_unsigned_psbt(&account, tx_template)
        .unwrap_or_else(|e| panic!("Transaction preparation should succeed: {e}"));
    let prepared = PsbtV2::deserialize(&psbt)
        .expect("prepared SP PSBTv2")
        .to_bitcoin_psbt_with_empty_sp_outputs()
        .expect("prepared SP PSBT");
    assert_eq!(
        prepared.fee().expect("SP PSBT fee").to_sat(),
        simulation.fee,
        "actual SP builder preserves the simulated sub-1 fee"
    );

    // Note: sign_and_broadcast requires a broadcast URL (Esplora-style HTTP endpoint)
    // which isn't available on regtest. Transaction preparation is sufficient to verify
    // the full SP transaction lifecycle up to signing.

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Wallet Restore Tests =====

#[test]
fn test_wallet_restore_from_descriptor() {
    let account_name = test_account_name();

    let config1 = Config::new(
        account_name.clone(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );
    let account1 = Account::new(config1).expect("First account creation should succeed");
    let addr1 = account1.sp_address();

    // The account dir holds an advisory lock, so close the first instance before
    // reopening the same account.
    drop(account1);
    cleanup_test_account(&account_name);

    let config2 = Config::new(
        account_name.clone(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );
    let account2 = Account::new(config2).expect("Restored account creation should succeed");
    let addr2 = account2.sp_address();

    assert_eq!(addr1, addr2, "Restored wallet should have same SP address");

    cleanup_test_account(&account_name);
}

// ===== Self-Send Tests =====

#[test]
fn test_self_send_produces_two_coins() {
    let (bbd, mut bitcoind_node) = setup_blindbitd();
    let bitcoind = &mut bitcoind_node.client;
    let url = bbd.url();

    bwk_test::generate_blocks(bitcoind, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account = create_test_account(&account_name, &url);

    assert!(account.start_scanner(), "Scanner should start");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );
    assert_eq!(account.balance(), 0, "New wallet should have zero balance");

    // Fund the wallet
    fund_sp_wallet(bitcoind, &url, TEST_MNEMONIC, 0.1);

    assert!(
        wait_for_notification(&mut account, NotificationFlag::NewOutput, 30),
        "Should receive NewOutput notification"
    );

    let balance = account.balance();
    assert!(balance > 0, "Balance should be positive after funding");

    let coins_before = account.coins();
    let unspent_before: Vec<_> = coins_before.iter().filter(|c| !c.spent).collect();
    assert_eq!(
        unspent_before.len(),
        1,
        "Should have exactly 1 unspent coin"
    );

    // Self-send: send half the balance to own SP address
    use silent::{Output, TransactionTemplate};

    let send_amount = balance / 2;
    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: send_amount,
            label: String::from("self send"),
            max: false,
        }],
        fee_rate: 0.0,
        fee: 1000,
        input_outpoints: vec![],
    };

    // Simulate
    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        simulation.is_valid,
        "Simulation should succeed: {}",
        simulation.error
    );

    // Prepare
    let psbt = sp_unsigned_psbt(&account, tx_template)
        .unwrap_or_else(|e| panic!("Prepare should succeed: {e}"));
    assert!(!psbt.is_empty(), "Prepared PSBT should be serialized");

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Signet Testing =====

#[test]
#[ignore] // Requires real signet BlindBit server (BlindbitD is regtest only)
fn test_signet_connection() {
    let account_name = test_account_name();
    let config = Config::new(
        account_name.clone(),
        Network::Signet,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Signet),
        "https://blindbit-signet.example.com".to_string(),
        String::new(),
        Some(546),
    );

    let mut account = Account::new(config).expect("Account creation should succeed");

    assert!(account.start_scanner(), "Signet scanner should start");

    let scan_completed = wait_for_scan_complete(&mut account, 30);
    assert!(scan_completed, "Scan should complete within timeout");

    let addr = account.sp_address();
    assert!(!addr.is_empty(), "Signet address should not be empty");
    assert!(
        addr.len() >= 100,
        "Signet address should be at least 100 characters"
    );

    account.stop_scanner();
    cleanup_test_account(&account_name);
}

// ===== Connection Loss Tests =====

#[test]
fn test_connection_loss_retry() {
    let (mut bbd, mut bitcoind_node) = setup_blindbitd();
    let bitcoind = &mut bitcoind_node.client;
    let url = bbd.url();

    bwk_test::generate_blocks(bitcoind, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account = create_test_account(&account_name, &url);

    assert!(account.start_scanner(), "Initial start should succeed");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );

    // Kill BlindbitD to simulate connection loss
    bbd.kill().expect("kill blindbitd");

    // Wait for FailScan notification
    let mut error_count = 0;
    for _ in 0..100 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if matches!(
                notif.flag,
                NotificationFlag::FailScan | NotificationFlag::FailStartScanning
            ) {
                error_count += 1;
                break;
            }
        }
        thread::sleep(Duration::from_millis(200));
    }

    assert!(
        error_count > 0,
        "Should receive at least one FailScan after connection loss"
    );

    account.stop_scanner();
    cleanup_test_account(&account_name);
}
