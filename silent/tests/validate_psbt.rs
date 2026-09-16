//! Integration tests for PSBT pre-signing validation.

mod common;

use common::{
    cleanup_test_account, create_test_account_with_electrum, fund_sp_wallet,
    setup_blindbitd_with_electrum, test_account_name, wait_for_scan_complete,
    wait_for_sync_and_index, TEST_MNEMONIC,
};
use silent::{Output, TransactionTemplate};

#[test]
fn test_validate_before_sign_success() {
    let account_name = test_account_name();
    let (bbd, mut bitcoind_node, electrum_url) = setup_blindbitd_with_electrum();
    let bitcoind = &mut bitcoind_node.client;
    let bbd_url = bbd.url();

    bwk_utils::test::generate_blocks(bitcoind, 101);
    wait_for_sync_and_index(&bbd_url, 101);

    let mut account = create_test_account_with_electrum(&account_name, &bbd_url, &electrum_url);
    assert!(account.start_scanner(), "Scanner should start");
    fund_sp_wallet(bitcoind, &bbd_url, TEST_MNEMONIC, 0.1);
    assert!(
        wait_for_scan_complete(&mut account, 60),
        "Scan should complete"
    );

    let balance = account.balance();
    assert!(balance > 0, "Should have balance after funding");

    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: account.sp_address(),
            amount: balance / 2,
            label: String::from("test psbt v2 validation"),
            max: false,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "Prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    let validation = account.validate_before_sign(&psbt);
    assert!(
        validation.is_ok,
        "Validation should succeed: {}",
        validation.error
    );
    assert!(
        validation.is_valid,
        "Validation should pass: {}",
        validation.issues
    );
    assert_eq!(validation.reused_output_count, 0);
    assert_eq!(validation.spent_input_count, 0);

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}

#[test]
fn test_validate_before_sign_detects_reused_output() {
    let account_name = test_account_name();
    let (bbd, mut bitcoind_node, electrum_url) = setup_blindbitd_with_electrum();
    let bitcoind = &mut bitcoind_node.client;
    let bbd_url = bbd.url();

    bwk_utils::test::generate_blocks(bitcoind, 101);
    wait_for_sync_and_index(&bbd_url, 101);

    let mut account = create_test_account_with_electrum(&account_name, &bbd_url, &electrum_url);
    assert!(account.start_scanner(), "Scanner should start");
    fund_sp_wallet(bitcoind, &bbd_url, TEST_MNEMONIC, 0.1);
    assert!(
        wait_for_scan_complete(&mut account, 60),
        "Scan should complete"
    );

    let balance = account.balance();
    assert!(balance > 0, "Should have balance after funding");

    // Pay this address once from outside the wallet, so its history is
    // already non-empty by the time the wallet targets it too.
    let reused_address = bitcoind.new_address().expect("new address");
    bwk_utils::test::send(bitcoind, reused_address.clone(), 0.01).expect("fund reused address");
    bwk_utils::test::generate_blocks(bitcoind, 2);
    let height = bwk_utils::test::get_height(bitcoind);
    wait_for_sync_and_index(&bbd_url, height as u32);

    let tx_template = TransactionTemplate {
        outputs: vec![Output {
            address: reused_address.to_string(),
            amount: balance / 2,
            label: String::from("test psbt v2 reused output"),
            max: false,
        }],
        fee_rate: 1.0,
        fee: 0,
        input_outpoints: vec![],
    };

    let psbt = account.prepare_transaction(tx_template);
    assert!(
        psbt.is_ok(),
        "Prepare should succeed: {}",
        psbt.get_psbt_error()
    );

    let validation = account.validate_before_sign(&psbt);
    assert!(
        validation.is_ok,
        "Validation should succeed: {}",
        validation.error
    );
    assert!(!validation.is_valid, "Validation should flag the reuse");
    assert_eq!(validation.reused_output_count, 1);
    assert_eq!(validation.spent_input_count, 0);

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}
