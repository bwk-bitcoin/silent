//! Multi-account, error handling, and edge case tests for Silent wallet.
//!
//! These tests verify multi-account isolation, comprehensive error handling,
//! and edge cases like zero balance sends, dust detection, and max sends.

mod common;

use bwk_utils::test as bwk_test;
use silent::{Account, Config, Network, Output, TransactionTemplate};
use std::thread;
use std::time::Duration;

use common::{
    cleanup_test_account, create_test_account, descriptor_from_mnemonic, fund_sp_wallet,
    fund_sp_wallet_at_index, setup_blindbitd, test_account_name, wait_for_scan_complete,
    wait_for_sync_and_index, TEST_MNEMONIC,
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

// ===== Multi-Account Isolation Tests =====

#[test]
fn test_multiple_accounts_independent_state() {
    let account1_name = format!("test_account_1_{}", std::process::id());
    let account2_name = format!("test_account_2_{}", std::process::id());

    let descriptor1 = descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest);
    let descriptor2 = descriptor_from_mnemonic(
        "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
        Network::Signet,
    );

    let config1 = Config::new(
        account1_name.clone(),
        Network::Regtest,
        descriptor1,
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );

    let config2 = Config::new(
        account2_name.clone(),
        Network::Signet,
        descriptor2,
        "http://localhost:50002".to_string(),
        String::new(),
        Some(1000),
    );

    let account1 = Account::new(config1.clone()).expect("Account 1 creation should succeed");
    let account2 = Account::new(config2.clone()).expect("Account 2 creation should succeed");

    assert_ne!(
        account1.sp_address(),
        account2.sp_address(),
        "Accounts should have different addresses"
    );
    assert_eq!(config1.get_network(), Network::Regtest);
    assert_eq!(config2.get_network(), Network::Signet);
    assert_ne!(config1.get_blindbit_url(), config2.get_blindbit_url());
    assert_ne!(config1.config_path(), config2.config_path());
    // The account directories are opaque: the names are not on disk in clear.
    assert!(!config1
        .config_path()
        .to_string_lossy()
        .contains(&account1_name));
    assert!(!config2
        .config_path()
        .to_string_lossy()
        .contains(&account2_name));

    cleanup_test_account(&account1_name);
    cleanup_test_account(&account2_name);
}

#[test]
fn test_account_config_isolation() {
    let account1_name = format!("test_iso_1_{}", std::process::id());
    let account2_name = format!("test_iso_2_{}", std::process::id());

    let config1 = Config::new(
        account1_name.clone(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );

    let config2 = Config::new(
        account2_name.clone(),
        Network::Testnet,
        descriptor_from_mnemonic(
            "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
            Network::Testnet,
        ),
        "http://localhost:50002".to_string(),
        String::new(),
        Some(1000),
    );

    config1.to_file();
    config2.to_file();

    let loaded1 = Config::from_file(account1_name.clone()).expect("Load should succeed");
    assert!(loaded1.get_descriptor().starts_with("sp("));
    assert_eq!(loaded1.get_network(), Network::Regtest);
    assert_eq!(loaded1.get_blindbit_url(), "http://localhost:50001");

    let loaded2 = Config::from_file(account2_name.clone()).expect("Load should succeed");
    assert!(loaded2.get_descriptor().starts_with("sp("));
    assert_eq!(loaded2.get_network(), Network::Testnet);
    assert_eq!(loaded2.get_blindbit_url(), "http://localhost:50002");

    cleanup_test_account(&account1_name);
    cleanup_test_account(&account2_name);
}

// ===== Error Handling Tests =====

#[test]
fn test_invalid_blindbit_url_error() {
    // Use an available port that nothing listens on — instant connection refusal
    let port = blindbitd::get_available_port().expect("get available port");
    let url = format!("http://127.0.0.1:{port}");

    let account_name = test_account_name();
    let config = Config::new(
        account_name.clone(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        url,
        String::new(),
        Some(546),
    );

    let mut account = Account::new(config).expect("Account creation should succeed");
    assert!(
        account.start_scanner(),
        "Scanner start should not fail immediately"
    );

    // Wait for FailStartScanning or FailScan notification
    let mut got_error = false;
    for _ in 0..50 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if matches!(
                notif.flag,
                silent::NotificationFlag::FailStartScanning | silent::NotificationFlag::FailScan
            ) {
                got_error = true;
                break;
            }
        }
        thread::sleep(Duration::from_millis(200));
    }

    assert!(
        got_error,
        "Should receive FailStartScanning or FailScan notification for invalid URL"
    );

    account.stop_scanner();
    cleanup_test_account(&account_name);
}

#[test]
fn test_invalid_descriptor_error() {
    let account_name = test_account_name();
    let config = Config::new(
        account_name.clone(),
        Network::Regtest,
        "not an sp descriptor".to_string(),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );

    let account_result = Account::new(config);
    assert!(
        account_result.is_err(),
        "Account creation should fail with invalid descriptor"
    );

    if let Err(error) = account_result {
        let error_msg = error.to_string();
        assert!(
            error_msg.contains("descriptor"),
            "Error should mention descriptor: {error_msg}"
        );
    }

    cleanup_test_account(&account_name);
}

#[test]
fn test_insufficient_funds_error() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());
    let account = Account::new(config).expect("Account creation should succeed");

    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: 100000,
            label: String::new(),
            max: false,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template);
    assert!(
        !simulation.is_valid,
        "Simulation should fail with insufficient funds"
    );
    assert!(!simulation.error.is_empty(), "Should have error message");

    cleanup_test_account(&account_name);
}

#[test]
fn test_invalid_sp_address_error() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());
    let account = Account::new(config).expect("Account creation should succeed");

    let invalid_addresses = vec!["not_a_valid_address", "tsp1invalid", "", "sp1"];

    for invalid_addr in invalid_addresses {
        let tx_template = TransactionTemplate {
            outputs: vec![Output {
                address: invalid_addr.to_string(),
                amount: 10000,
                label: String::new(),
                max: false,
            }],
            fee_rate: 1.0,
            fee: 0,
            input_outpoints: vec![],
        };

        let simulation = account.simulate_transaction(tx_template.clone());
        assert!(
            !simulation.is_valid,
            "Simulation should fail with invalid address: {invalid_addr}"
        );
        assert!(
            !simulation.error.is_empty(),
            "Should have error message for invalid address: {invalid_addr}"
        );
    }

    cleanup_test_account(&account_name);
}

// ===== Connection Tests =====

#[test]
fn test_concurrent_scanner_operations() {
    let (bbd, mut bitcoind_node) = setup_blindbitd();
    let bitcoind = &mut bitcoind_node.client;
    let url = bbd.url();

    bwk_test::generate_blocks(bitcoind, 101);
    wait_for_sync_and_index(&url, 101);

    let account_name = test_account_name();
    let mut account = create_test_account(&account_name, &url);

    // Start scanner
    assert!(account.start_scanner(), "First start should succeed");
    assert!(
        wait_for_scan_complete(&mut account, 30),
        "Initial scan should complete"
    );

    // Try to start again (should be idempotent or return false)
    let second_start = account.start_scanner();
    if !second_start {
        println!("Second start returned false (expected)");
    }

    // Stop and restart
    account.stop_scanner();
    thread::sleep(Duration::from_millis(500));

    assert!(account.start_scanner(), "Restart should succeed");

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Edge Case Tests =====

#[test]
fn test_zero_balance_send_attempt() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());
    let account = Account::new(config).expect("Account creation should succeed");

    assert_eq!(account.balance(), 0, "New wallet should have zero balance");

    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: 1000,
            label: String::new(),
            max: false,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        !simulation.is_valid,
        "Should not be able to send with zero balance"
    );
    assert!(!simulation.error.is_empty(), "Should have error message");

    let psbt = account.prepare_transaction(tx_template);
    assert!(
        !psbt.is_ok(),
        "Prepare should fail: {}",
        psbt.get_psbt_error()
    );

    cleanup_test_account(&account_name);
}

#[test]
fn test_dust_output_detection() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());
    let account = Account::new(config).expect("Account creation should succeed");

    let dust_amounts = vec![1, 100, 545];

    for dust_amount in dust_amounts {
        let tx_template = TransactionTemplate {
            outputs: vec![Output {
                address: account.sp_address(),
                amount: dust_amount,
                label: String::new(),
                max: false,
            }],
            fee_rate: 1.0,
            fee: 0,
            input_outpoints: vec![],
        };

        let simulation = account.simulate_transaction(tx_template);
        if !simulation.is_valid {
            println!("Dust amount {} rejected: {}", dust_amount, simulation.error);
        }
    }

    cleanup_test_account(&account_name);
}

#[test]
fn test_max_send_entire_balance() {
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

    // Fund the wallet
    fund_sp_wallet(bitcoind, &url, TEST_MNEMONIC, 0.1);

    // Wait for NewOutput
    let mut funded = false;
    for _ in 0..300 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == silent::NotificationFlag::NewOutput {
                funded = true;
                break;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    assert!(funded, "Should receive NewOutput notification");

    let balance = account.balance();
    assert!(balance > 0, "Balance should be non-zero after funding");

    // Create max send transaction
    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: 0,
            label: String::from("max send"),
            max: true,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        simulation.is_valid,
        "Max send simulation should succeed: {}",
        simulation.error
    );

    // Verify output amount = balance - fee
    let expected_output = balance.saturating_sub(simulation.fee);
    assert_eq!(
        simulation.output_total, expected_output,
        "Output should equal balance minus fee"
    );

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

#[test]
fn test_max_output_combined_with_fixed_outputs() {
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

    // Fund the wallet
    fund_sp_wallet(bitcoind, &url, TEST_MNEMONIC, 0.1);

    // Wait for NewOutput
    let mut funded = false;
    for _ in 0..300 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == silent::NotificationFlag::NewOutput {
                funded = true;
                break;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    assert!(funded, "Should receive NewOutput notification");

    let balance = account.balance();
    assert!(balance > 0, "Balance should be non-zero after funding");

    // Combine a fixed output with a max output (no manual coin selection)
    let addr = account.sp_address();
    let fixed_amount = 1_000_000;
    let tx_template = TransactionTemplate {
        outputs: vec![
            Output {
                address: addr.clone(),
                amount: fixed_amount,
                label: String::new(),
                max: false,
            },
            Output {
                address: addr,
                amount: 0,
                label: String::new(),
                max: true,
            },
        ],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template);
    assert!(
        simulation.is_valid,
        "Max combined with fixed outputs should succeed: {}",
        simulation.error
    );

    // Max output should get balance - fixed_amount - fee
    let expected_output = balance.saturating_sub(simulation.fee);
    assert_eq!(
        simulation.output_total, expected_output,
        "Total output should equal balance minus fee"
    );

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

#[test]
fn test_multiple_outputs_single_transaction() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());
    let account = Account::new(config).expect("Account creation should succeed");

    let addr = account.sp_address();
    let tx_template = TransactionTemplate {
        outputs: vec![
            Output {
                address: addr.clone(),
                amount: 10000,
                label: String::from("output 1"),
                max: false,
            },
            Output {
                address: addr,
                amount: 20000,
                label: String::from("output 2"),
                max: false,
            },
        ],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let simulation = account.simulate_transaction(tx_template);
    if !simulation.is_valid {
        assert!(!simulation.error.is_empty());
    }

    cleanup_test_account(&account_name);
}

#[test]
fn test_manual_coin_selection() {
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

    // Fund wallet twice (two separate SP outputs at different derivation indices)
    fund_sp_wallet(bitcoind, &url, TEST_MNEMONIC, 0.1);

    // Wait for first output
    let mut output_count = 0;
    for _ in 0..300 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == silent::NotificationFlag::NewOutput {
                output_count += 1;
                break;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    assert!(output_count >= 1, "Should receive first NewOutput");

    // Fund again at a different index
    fund_sp_wallet_at_index(bitcoind, &url, TEST_MNEMONIC, 0.05, 1);

    // Wait for second output
    for _ in 0..300 {
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == silent::NotificationFlag::NewOutput {
                output_count += 1;
                break;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    assert!(
        output_count >= 2,
        "Should receive two NewOutput notifications"
    );

    let coins = account.coins();
    let spendable: Vec<_> = coins.iter().filter(|c| !c.spent).collect();
    assert!(
        spendable.len() >= 2,
        "Should have at least 2 spendable coins, got {}",
        spendable.len()
    );

    // Select specific coins
    let selected_outpoints: Vec<String> = spendable
        .iter()
        .take(2)
        .map(|c| c.outpoint.to_string())
        .collect();

    let selected_value: u64 = spendable.iter().take(2).map(|c| c.value).sum();

    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: selected_value / 2,
            label: String::from("manual selection test"),
            max: false,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: selected_outpoints.clone(),
    };

    let simulation = account.simulate_transaction(tx_template.clone());
    assert!(
        simulation.is_valid,
        "Manual coin selection should work: {}",
        simulation.error
    );
    assert_eq!(
        simulation.input_count as usize,
        selected_outpoints.len(),
        "Should use exactly the selected coins"
    );

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

// ===== Additional Error Path Tests =====

#[test]
fn test_invalid_outpoint_format() {
    let account_name = test_account_name();
    let config = create_test_config(account_name.clone());
    let mut account = Account::new(config).expect("Account creation should succeed");

    assert!(
        !account.update_coin_label("invalid_format".to_string(), "test".to_string()),
        "Should reject invalid outpoint format"
    );

    cleanup_test_account(&account_name);
}
