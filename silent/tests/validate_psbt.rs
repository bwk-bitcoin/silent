//! Integration tests for PSBT pre-signing validation.

mod common;

use common::{
    cleanup_test_account, create_test_account_with_electrum, fund_sp_wallet,
    setup_blindbitd_with_electrum, test_account_name, wait_for_scan_complete,
    wait_for_sync_and_index, TEST_MNEMONIC,
};
use silent::{Output, TransactionTemplate};

#[test]
fn test_validate_before_sign_psbt_v2_not_available() {
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
    assert!(!validation.is_ok, "Validation should report unavailable");
    assert_eq!(
        validation.error,
        "PSBT v2 Electrum validation is not available yet"
    );
    assert!(!validation.is_valid, "Validation should not pass");
    assert_eq!(validation.reused_output_count, 0);
    assert_eq!(validation.spent_input_count, 0);

    account.stop_scanner();
    cleanup_test_account(&account_name);
    drop(bbd);
}
