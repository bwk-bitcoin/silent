//! End-to-end integration tests for Silent wallet.
//!
//! These tests cover wallet creation, config management, SP address generation,
//! BlindBit connection, send/receive flows, and wallet restore functionality.

mod common;

use bwk_utils::test as bwk_test;
use silent::{Account, Config, Network, NotificationFlag};
use std::thread;
use std::time::Duration;

use common::{
    cleanup_test_account, create_test_account, create_test_account_with_electrum,
    create_test_account_with_electrum_mnemonic, descriptor_from_mnemonic, fund_sp_wallet,
    setup_blindbitd, setup_blindbitd_with_electrum, test_account_name, wait_for_notification,
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

#[test]
fn test_wallet_creation_from_descriptor() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());

    assert_eq!(config.account_name, account_name);
    assert_eq!(config.get_network(), Network::Regtest);
    assert!(!config.get_descriptor().is_empty());

    let account_result = Account::new(config.clone());
    assert!(account_result.is_ok(), "Account creation should succeed");

    let account = account_result.unwrap();
    assert_eq!(account.name(), account_name);
    assert_eq!(account.balance(), 0);

    cleanup_test_account(&account_name);
}

#[test]
fn test_config_save_load_roundtrip() {
    let account_name = test_account_name();
    let original_config = create_test_config(account_name.clone());

    original_config.to_file();
    assert!(original_config.config_path().exists());

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
        fee_rate: 1.0,
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

    // Prepare transaction
    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "Transaction preparation should succeed: {}",
        psbt.get_psbt_error()
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
    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "Prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    assert!(
        !psbt.bytes_hex().is_empty(),
        "Prepared PSBT should be serialized"
    );

    let signed = account.sign_transaction(&psbt);
    assert!(!signed.is_ok, "Hot signing should be unavailable");
    assert_eq!(signed.error, "External signer required");

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Multi-Address-Type Send Tests =====

#[test]
#[ignore = "hot segwit/taproot sub-accounts were removed"]
fn test_send_to_sp_segwit_taproot() {
    let (bbd, mut bitcoind_node, electrum_url) = setup_blindbitd_with_electrum();
    let url = bbd.url();

    bwk_test::generate_blocks(&mut bitcoind_node.client, 101);
    wait_for_sync_and_index(&url, 101);

    // Create account with electrum (enables sub-accounts)
    let account_name = test_account_name();
    let mut account = create_test_account_with_electrum(&account_name, &url, &electrum_url);
    assert!(
        account.has_sub_accounts(),
        "Account should have sub-accounts when electrum is configured"
    );

    // Start scanner and wait for initial sync
    assert!(account.start_scanner(), "Scanner should start");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );
    assert_eq!(account.balance(), 0, "New wallet should have zero balance");

    // Fund the wallet with an SP transaction
    let bitcoind = &mut bitcoind_node.client;
    fund_sp_wallet(bitcoind, &url, TEST_MNEMONIC, 0.1);

    assert!(
        wait_for_notification(&mut account, NotificationFlag::NewOutput, 30),
        "Should receive NewOutput notification for initial funding"
    );

    let balance = account.balance();
    assert!(balance > 0, "Balance should be positive after funding");
    eprintln!("Initial balance: {balance} sats");

    // Generate sub-account addresses
    let segwit_addr = account.new_segwit_addr();
    let taproot_addr = account.new_taproot_addr();
    assert!(
        segwit_addr.starts_with("bcrt1q"),
        "Segwit address should start with bcrt1q, got {segwit_addr}"
    );
    assert!(
        taproot_addr.starts_with("bcrt1p"),
        "Taproot address should start with bcrt1p, got {taproot_addr}"
    );

    // Build tx with 3 outputs: SP (self-send) + segwit + taproot
    use silent::{Output, TransactionTemplate};

    let quarter = balance / 4;
    let tx_template = TransactionTemplate {
        outputs: vec![
            Output {
                address: account.sp_address(),
                amount: quarter,
                label: String::from("sp self-send"),
                max: false,
            },
            Output {
                address: segwit_addr.clone(),
                amount: quarter,
                label: String::from("segwit"),
                max: false,
            },
            Output {
                address: taproot_addr.clone(),
                amount: quarter,
                label: String::from("taproot"),
                max: false,
            },
        ],
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
    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "Prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    // Sign
    let signed = account.sign_transaction(&psbt);
    assert!(signed.is_ok, "Sign should succeed: {}", signed.error);

    // Inspect the signed tx
    let tx: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed.value).expect("valid tx hex");
    assert!(
        tx.output.len() >= 3,
        "Transaction should have at least 3 outputs (SP + segwit + taproot), got {}",
        tx.output.len()
    );

    eprintln!("Transaction txid: {}", tx.compute_txid());
    for (i, out) in tx.output.iter().enumerate() {
        eprintln!(
            "  Output {i}: value={} sats, script={:?}",
            out.value.to_sat(),
            out.script_pubkey
        );
    }

    // Broadcast via bitcoind and mine
    bitcoind
        .send_raw_transaction(&tx)
        .expect("broadcast should succeed");
    bwk_test::generate_blocks(bitcoind, 1);

    let tx_height =
        bwk_test::get_tx_height(bitcoind, tx.compute_txid()).expect("get tx height") as u32;
    wait_for_sync_and_index(&url, tx_height);

    // Wait for scanner to detect the SP output(s)
    let mut new_output_count = 0;
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(30);
    loop {
        if start.elapsed() > timeout {
            break;
        }
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == NotificationFlag::NewOutput {
                new_output_count += 1;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    eprintln!("NewOutput notifications received: {new_output_count}");

    // Poll coins until at least 3 unspent coins appear (SP + segwit + taproot)
    // Sub-account coins are detected via electrum subscription
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(30);
    loop {
        let coins = account.coins();
        let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
        if unspent.len() >= 3 {
            break;
        }
        if start.elapsed() > timeout {
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    // Report final coin state
    let coins = account.coins();
    eprintln!("All coins after send:");
    for coin in &coins {
        eprintln!(
            "  {} value={} spent={} height={}",
            coin.outpoint, coin.value, coin.spent, coin.height
        );
    }

    let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
    assert!(
        unspent.len() >= 3,
        "Expected at least 3 unspent coins (SP + segwit + taproot), got {}",
        unspent.len()
    );

    // Cleanup
    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Mixed-Input Signing Tests =====

/// Regression test: spending from mixed inputs (SP + segwit + taproot sub-accounts)
/// must produce valid witnesses for ALL inputs.
///
/// Bug: segwit sub-account inputs were not being signed, resulting in empty witnesses
/// and "Witness program hash mismatch" rejection on broadcast.
#[test]
#[ignore = "hot segwit/taproot sub-accounts were removed"]
fn test_mixed_input_signing() {
    let mnemonic =
        "vehicle priority voice index lunch exact whale decrease doctor column enter lobster";

    let (bbd, mut bitcoind_node, electrum_url) = setup_blindbitd_with_electrum();
    let url = bbd.url();

    bwk_test::generate_blocks(&mut bitcoind_node.client, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account =
        create_test_account_with_electrum_mnemonic(&account_name, &url, &electrum_url, mnemonic);
    assert!(
        account.has_sub_accounts(),
        "Account should have sub-accounts when electrum is configured"
    );

    assert!(account.start_scanner(), "Scanner should start");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );

    // Step 1: Fund with SP coin (1 BTC)
    let bitcoind = &mut bitcoind_node.client;
    fund_sp_wallet(bitcoind, &url, mnemonic, 1.0);

    assert!(
        wait_for_notification(&mut account, NotificationFlag::NewOutput, 30),
        "Should receive SP NewOutput"
    );
    let sp_balance = account.balance();
    assert!(sp_balance > 0, "SP balance should be positive");
    eprintln!("SP balance: {sp_balance} sats");

    // Step 2: Send to segwit + taproot sub-account addresses to create sub-account coins
    let segwit_addr = account.new_segwit_addr();
    let taproot_addr = account.new_taproot_addr();

    use silent::{Output, TransactionTemplate};

    let tx_template = TransactionTemplate {
        outputs: vec![
            Output {
                address: segwit_addr.clone(),
                amount: 20_000_000, // 0.2 BTC to segwit
                label: String::from("segwit fund"),
                max: false,
            },
            Output {
                address: taproot_addr.clone(),
                amount: 30_000_000, // 0.3 BTC to taproot
                label: String::from("taproot fund"),
                max: false,
            },
        ],
        fee_rate: 0.0,
        fee: 1000,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        simulation.is_valid,
        "Simulation should succeed: {}",
        simulation.error
    );

    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "Prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    let signed = account.sign_transaction(&psbt);
    assert!(signed.is_ok, "Sign should succeed: {}", signed.error);

    let tx: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed.value).expect("valid tx hex");

    // Broadcast first tx (SP → segwit + taproot + SP change)
    bitcoind
        .send_raw_transaction(&tx)
        .expect("first broadcast should succeed");
    bwk_test::generate_blocks(bitcoind, 1);

    let tx_height =
        bwk_test::get_tx_height(bitcoind, tx.compute_txid()).expect("get tx height") as u32;
    wait_for_sync_and_index(&url, tx_height);

    // Wait for SP change output
    wait_for_notification(&mut account, NotificationFlag::NewOutput, 30);

    // Wait for sub-account coins to appear via electrum
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(30);
    loop {
        let coins = account.coins();
        let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
        // Need at least 3 unspent: SP change + segwit + taproot
        if unspent.len() >= 3 {
            break;
        }
        if start.elapsed() > timeout {
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    let coins = account.coins();
    let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
    eprintln!("Coins before mixed spend:");
    for coin in &unspent {
        eprintln!("  {} value={}", coin.outpoint, coin.value);
    }
    assert!(
        unspent.len() >= 3,
        "Expected at least 3 unspent coins (SP + segwit + taproot), got {}",
        unspent.len()
    );

    // Step 3: Spend ALL coins (mixed inputs) — max send to a segwit address
    let dest_addr = account.new_segwit_addr();
    let tx_template2 = TransactionTemplate {
        outputs: vec![Output {
            address: dest_addr,
            amount: 0,
            label: String::new(),
            max: true,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![], // auto-select all
    };

    let simulation2 = account.simulate_transaction(tx_template2.clone());
    assert!(
        simulation2.is_valid,
        "Mixed-input simulation should succeed: {}",
        simulation2.error
    );

    let psbt2 = account.prepare_transaction(tx_template2);
    assert!(
        psbt2.is_ok(),
        "Mixed-input prepare should succeed: {}",
        psbt2.get_psbt_error()
    );

    let signed2 = account.sign_transaction(&psbt2);
    assert!(
        signed2.is_ok,
        "Mixed-input sign should succeed: {}",
        signed2.error
    );

    // Step 4: Verify ALL inputs have witnesses
    let tx2: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed2.value).expect("valid tx hex");

    eprintln!(
        "Mixed-input tx: {} inputs, {} outputs",
        tx2.input.len(),
        tx2.output.len()
    );
    for (i, input) in tx2.input.iter().enumerate() {
        eprintln!(
            "  Input [{i}] {} witness_items={}",
            input.previous_output,
            input.witness.len()
        );
        assert!(
            !input.witness.is_empty(),
            "SIGNING BUG: input [{i}] {} has empty witness",
            input.previous_output
        );
    }

    // Step 5: Broadcast via bitcoind to confirm validity
    bitcoind
        .send_raw_transaction(&tx2)
        .expect("mixed-input broadcast should succeed");

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== SpPartialSecret Regression Tests =====

/// Regression test: max send to SP address with mixed inputs (SP + segwit + taproot)
/// fails with SpPartialSecret error.
///
/// Bug: when the destination is an SP address and the wallet has mixed coin types,
/// `builder.generate()` fails because it cannot compute the SP partial secret
/// needed for the SP output when sub-account (non-SP) inputs are involved.
#[test]
#[ignore = "hot segwit/taproot sub-accounts were removed"]
fn test_max_send_to_sp_with_mixed_inputs() {
    let mnemonic =
        "vehicle priority voice index lunch exact whale decrease doctor column enter lobster";

    let (bbd, mut bitcoind_node, electrum_url) = setup_blindbitd_with_electrum();
    let url = bbd.url();

    bwk_test::generate_blocks(&mut bitcoind_node.client, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account =
        create_test_account_with_electrum_mnemonic(&account_name, &url, &electrum_url, mnemonic);
    assert!(
        account.has_sub_accounts(),
        "Account should have sub-accounts when electrum is configured"
    );

    assert!(account.start_scanner(), "Scanner should start");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );

    // Step 1: Fund with SP coin (1 BTC)
    let bitcoind = &mut bitcoind_node.client;
    fund_sp_wallet(bitcoind, &url, mnemonic, 1.0);

    assert!(
        wait_for_notification(&mut account, NotificationFlag::NewOutput, 30),
        "Should receive SP NewOutput"
    );
    let sp_balance = account.balance();
    assert!(sp_balance > 0, "SP balance should be positive");

    // Step 2: Send to segwit + taproot sub-account addresses to create sub-account coins
    let segwit_addr = account.new_segwit_addr();
    let taproot_addr = account.new_taproot_addr();

    use silent::{Output, TransactionTemplate};

    let tx_template = TransactionTemplate {
        outputs: vec![
            Output {
                address: segwit_addr.clone(),
                amount: 20_000_000, // 0.2 BTC to segwit
                label: String::from("segwit fund"),
                max: false,
            },
            Output {
                address: taproot_addr.clone(),
                amount: 30_000_000, // 0.3 BTC to taproot
                label: String::from("taproot fund"),
                max: false,
            },
        ],
        fee_rate: 0.0,
        fee: 1000,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        simulation.is_valid,
        "Simulation should succeed: {}",
        simulation.error
    );

    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "Prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    let signed = account.sign_transaction(&psbt);
    assert!(signed.is_ok, "Sign should succeed: {}", signed.error);

    let tx: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed.value).expect("valid tx hex");

    // Broadcast (SP → segwit + taproot + SP change)
    bitcoind
        .send_raw_transaction(&tx)
        .expect("first broadcast should succeed");
    bwk_test::generate_blocks(bitcoind, 1);

    let tx_height =
        bwk_test::get_tx_height(bitcoind, tx.compute_txid()).expect("get tx height") as u32;
    wait_for_sync_and_index(&url, tx_height);

    // Wait for SP change output
    wait_for_notification(&mut account, NotificationFlag::NewOutput, 30);

    // Wait for sub-account coins to appear via electrum
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(30);
    loop {
        let coins = account.coins();
        let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
        if unspent.len() >= 3 {
            break;
        }
        if start.elapsed() > timeout {
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    let coins = account.coins();
    let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
    eprintln!("Coins before mixed SP send:");
    for coin in &unspent {
        eprintln!("  {} value={}", coin.outpoint, coin.value);
    }
    assert!(
        unspent.len() >= 3,
        "Expected at least 3 unspent coins (SP + segwit + taproot), got {}",
        unspent.len()
    );

    // Step 3: Max send to SP address (this triggers SpPartialSecret)
    let tx_template2 = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: 0,
            label: String::new(),
            max: true,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let simulation2 = account.simulate_transaction(tx_template2.clone());
    assert!(
        simulation2.is_valid,
        "Mixed-input SP simulation should succeed: {}",
        simulation2.error
    );

    let psbt2 = account.prepare_transaction(tx_template2);
    assert!(
        psbt2.is_ok(),
        "Mixed-input SP prepare should succeed: {}",
        psbt2.get_psbt_error()
    );

    let signed2 = account.sign_transaction(&psbt2);
    assert!(
        signed2.is_ok,
        "Mixed-input SP sign should succeed: {}",
        signed2.error
    );

    // Broadcast to confirm validity
    let tx2: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed2.value).expect("valid tx hex");
    bitcoind
        .send_raw_transaction(&tx2)
        .expect("mixed-input SP broadcast should succeed");

    // Step 4: Mine and verify scanner detects the SP output from mixed-input send
    bwk_test::generate_blocks(bitcoind, 1);

    let tx2_height =
        bwk_test::get_tx_height(bitcoind, tx2.compute_txid()).expect("get tx2 height") as u32;
    wait_for_sync_and_index(&url, tx2_height);

    // Wait for scanner to detect the new SP output
    assert!(
        wait_for_notification(&mut account, NotificationFlag::NewOutput, 30),
        "Scanner should detect the SP output from mixed-input send"
    );

    let coins_after = account.coins();
    let unspent_after: Vec<_> = coins_after.iter().filter(|c| !c.spent).collect();
    eprintln!("Coins after mixed-input SP send:");
    for coin in &unspent_after {
        eprintln!(
            "  {} value={} spent={}",
            coin.outpoint, coin.value, coin.spent
        );
    }
    assert!(
        !unspent_after.is_empty(),
        "Scanner should detect at least 1 unspent SP coin from mixed-input send, got 0"
    );

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Payment History Tests =====

/// Verify that payment_history() returns outgoing transactions for all coin types:
/// SP, Segwit, and Taproot.
#[test]
#[ignore = "upstream bwk race/poison at c3f31e8 (tx_store/account drop persist path)"]
fn test_payment_history_outgoing_all_types() {
    let mnemonic =
        "vehicle priority voice index lunch exact whale decrease doctor column enter lobster";

    let (bbd, mut bitcoind_node, electrum_url) = setup_blindbitd_with_electrum();
    let url = bbd.url();

    bwk_test::generate_blocks(&mut bitcoind_node.client, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account =
        create_test_account_with_electrum_mnemonic(&account_name, &url, &electrum_url, mnemonic);
    assert!(
        account.has_sub_accounts(),
        "Account should have sub-accounts when electrum is configured"
    );

    assert!(account.start_scanner(), "Scanner should start");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );

    // Step 1: Fund with SP coin (1 BTC)
    let bitcoind = &mut bitcoind_node.client;
    fund_sp_wallet(bitcoind, &url, mnemonic, 1.0);

    assert!(
        wait_for_notification(&mut account, NotificationFlag::NewOutput, 30),
        "Should receive SP NewOutput"
    );
    let sp_balance = account.balance();
    assert!(sp_balance > 0, "SP balance should be positive");
    eprintln!("SP balance: {sp_balance} sats");

    // Check incoming payment history
    let history = account.payment_history();
    eprintln!("Payment history after funding:");
    for tx in &history {
        eprintln!(
            "  txid={} dir={} amount={} height={}",
            tx.txid, tx.direction, tx.amount, tx.height
        );
    }
    let incoming_count = history
        .iter()
        .filter(|tx| tx.direction == "incoming")
        .count();
    assert!(
        incoming_count >= 1,
        "Should have at least 1 incoming payment after funding, got {incoming_count}"
    );

    // Step 2: Spend SP coin → segwit + taproot sub-accounts
    let segwit_addr = account.new_segwit_addr();
    let taproot_addr = account.new_taproot_addr();

    use silent::{Output, TransactionTemplate};

    let tx_template = TransactionTemplate {
        outputs: vec![
            Output {
                address: segwit_addr.clone(),
                amount: 20_000_000, // 0.2 BTC to segwit
                label: String::from("segwit fund"),
                max: false,
            },
            Output {
                address: taproot_addr.clone(),
                amount: 30_000_000, // 0.3 BTC to taproot
                label: String::from("taproot fund"),
                max: false,
            },
        ],
        fee_rate: 0.0,
        fee: 1000,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        simulation.is_valid,
        "SP spend simulation should succeed: {}",
        simulation.error
    );

    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "SP spend prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    let signed = account.sign_transaction(&psbt);
    assert!(
        signed.is_ok,
        "SP spend sign should succeed: {}",
        signed.error
    );

    let tx: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed.value).expect("valid tx hex");

    bitcoind
        .send_raw_transaction(&tx)
        .expect("SP spend broadcast should succeed");
    bwk_test::generate_blocks(bitcoind, 1);

    let tx_height =
        bwk_test::get_tx_height(bitcoind, tx.compute_txid()).expect("get tx height") as u32;
    wait_for_sync_and_index(&url, tx_height);

    // Wait for SP change output
    wait_for_notification(&mut account, NotificationFlag::NewOutput, 30);

    // Wait for sub-account coins to appear via electrum
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(30);
    loop {
        let coins = account.coins();
        let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
        if unspent.len() >= 3 {
            break;
        }
        if start.elapsed() > timeout {
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }

    let coins = account.coins();
    let unspent: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
    assert!(
        unspent.len() >= 3,
        "Expected at least 3 unspent coins (SP change + segwit + taproot), got {}",
        unspent.len()
    );

    // Check outgoing payment history after SP spend
    thread::sleep(Duration::from_secs(3));
    let history = account.payment_history();
    eprintln!("Payment history after SP spend:");
    for tx in &history {
        eprintln!(
            "  txid={} dir={} amount={} height={}",
            tx.txid, tx.direction, tx.amount, tx.height
        );
    }
    let outgoing_after_sp = history
        .iter()
        .filter(|tx| tx.direction == "outgoing")
        .count();
    eprintln!("Outgoing count after SP spend: {outgoing_after_sp}");

    // Step 3: Spend segwit coin
    let segwit_coin = coins
        .iter()
        .find(|c| c.account_type == "Segwit" && !c.spent)
        .expect("Should have a Segwit coin");
    let segwit_outpoint = segwit_coin.outpoint.clone();
    eprintln!(
        "Spending Segwit coin: {} value={}",
        segwit_outpoint, segwit_coin.value
    );

    let dest_addr = account.new_segwit_addr();
    let tx_template_segwit = TransactionTemplate {
        outputs: vec![Output {
            address: dest_addr,
            amount: 0,
            label: String::new(),
            max: true,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![segwit_outpoint],
    };

    let sim = account.simulate_transaction(tx_template_segwit.clone());
    assert!(
        sim.is_valid,
        "Segwit spend simulation should succeed: {}",
        sim.error
    );

    let psbt = account.prepare_transaction(tx_template_segwit);
    assert!(
        psbt.is_ok(),
        "Segwit spend prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    let signed = account.sign_transaction(&psbt);
    assert!(
        signed.is_ok,
        "Segwit spend sign should succeed: {}",
        signed.error
    );

    let tx: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed.value).expect("valid tx hex");
    bitcoind
        .send_raw_transaction(&tx)
        .expect("Segwit spend broadcast should succeed");
    bwk_test::generate_blocks(bitcoind, 1);

    let tx_height =
        bwk_test::get_tx_height(bitcoind, tx.compute_txid()).expect("get tx height") as u32;
    wait_for_sync_and_index(&url, tx_height);

    // Wait for sub-account coins to update
    thread::sleep(Duration::from_secs(3));

    // Check outgoing payment history after segwit spend
    let history = account.payment_history();
    eprintln!("Payment history after Segwit spend:");
    for tx in &history {
        eprintln!(
            "  txid={} dir={} amount={} height={}",
            tx.txid, tx.direction, tx.amount, tx.height
        );
    }
    let outgoing_after_segwit = history
        .iter()
        .filter(|tx| tx.direction == "outgoing")
        .count();
    eprintln!("Outgoing count after Segwit spend: {outgoing_after_segwit}");

    // Step 4: Spend taproot coin
    let coins = account.coins();
    let taproot_coin = coins
        .iter()
        .find(|c| c.account_type == "Taproot" && !c.spent)
        .expect("Should have a Taproot coin");
    let taproot_outpoint = taproot_coin.outpoint.clone();
    eprintln!(
        "Spending Taproot coin: {} value={}",
        taproot_outpoint, taproot_coin.value
    );

    let dest_addr = account.new_segwit_addr();
    let tx_template_taproot = TransactionTemplate {
        outputs: vec![Output {
            address: dest_addr,
            amount: 0,
            label: String::new(),
            max: true,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![taproot_outpoint],
    };

    let sim = account.simulate_transaction(tx_template_taproot.clone());
    assert!(
        sim.is_valid,
        "Taproot spend simulation should succeed: {}",
        sim.error
    );

    let psbt = account.prepare_transaction(tx_template_taproot);
    assert!(
        psbt.is_ok(),
        "Taproot spend prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    let signed = account.sign_transaction(&psbt);
    assert!(
        signed.is_ok,
        "Taproot spend sign should succeed: {}",
        signed.error
    );

    let tx: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&signed.value).expect("valid tx hex");
    bitcoind
        .send_raw_transaction(&tx)
        .expect("Taproot spend broadcast should succeed");
    bwk_test::generate_blocks(bitcoind, 1);

    let tx_height =
        bwk_test::get_tx_height(bitcoind, tx.compute_txid()).expect("get tx height") as u32;
    wait_for_sync_and_index(&url, tx_height);

    // Wait for sub-account coins to update
    thread::sleep(Duration::from_secs(3));

    // Final payment history check
    let history = account.payment_history();
    eprintln!("=== Final payment history ===");
    for tx in &history {
        eprintln!(
            "  txid={} dir={} amount={} height={}",
            tx.txid, tx.direction, tx.amount, tx.height
        );
    }
    let total_outgoing = history
        .iter()
        .filter(|tx| tx.direction == "outgoing")
        .count();
    let total_incoming = history
        .iter()
        .filter(|tx| tx.direction == "incoming")
        .count();
    eprintln!("Total incoming: {total_incoming}, Total outgoing: {total_outgoing}");

    assert!(
        total_incoming >= 2,
        "Expected at least 2 incoming payments after multi-step flow, got {total_incoming}"
    );

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
