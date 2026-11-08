//! Integration tests for PSBT pre-signing validation, and for the prepared PSBT going
//! through the rest of the SP signing flow: the hot key signs it, the account finalizes it.

mod common;

use base64::{engine::general_purpose::STANDARD, Engine};
use bitcoin::{consensus::encode::deserialize, Transaction};
use blindbitd::BlindbitD;
use bwk_sp::bwk_tx::bwk_psbt::PsbtV2;
use bwk_utils::test::corepc_node;
use common::{
    cleanup_test_account, create_test_account_with_electrum, descriptor_from_mnemonic,
    fund_sp_wallet, setup_blindbitd_with_electrum, test_account_name, wait_for_scan_complete,
    wait_for_sync_and_index, TEST_MNEMONIC,
};
use silent::{Account, Network, Output, TransactionTemplate};

const TEST_FINGERPRINT_HEX: &str = "73c5da0a";
const KEY_PASSWORD: &str = "password";

/// The test mnemonic's SP account, funded once and scanned, and the servers it runs
/// against.
struct Funded {
    name: String,
    account: Account,
    node: corepc_node::Node,
    bbd: BlindbitD,
}

fn funded() -> Funded {
    let name = test_account_name();
    let (bbd, mut node, electrum_url) = setup_blindbitd_with_electrum();
    let bbd_url = bbd.url();

    bwk_utils::test::generate_blocks(&mut node.client, 101);
    wait_for_sync_and_index(&bbd_url, 101);

    let mut account = create_test_account_with_electrum(&name, &bbd_url, &electrum_url);
    assert!(account.start_scanner(), "Scanner should start");
    fund_sp_wallet(&mut node.client, &bbd_url, TEST_MNEMONIC, 0.1);
    assert!(
        wait_for_scan_complete(&mut account, 60),
        "Scan should complete"
    );
    assert!(account.balance() > 0, "Should have balance after funding");
    Funded {
        name,
        account,
        node,
        bbd,
    }
}

impl Funded {
    /// The unsigned PSBT of half the balance to `address`.
    fn prepared(&self, address: String, label: &str) -> Vec<u8> {
        let tx_template = TransactionTemplate {
            outputs: vec![Output {
                address,
                amount: self.account.balance() / 2,
                label: label.into(),
                max: false,
            }],
            fee_rate: 1.0,
            fee: 0,
            input_outpoints: vec![],
        };
        common::sp_unsigned_psbt(&self.account, tx_template)
            .unwrap_or_else(|e| panic!("Prepare should succeed: {e}"))
    }

    fn finish(mut self) {
        self.account.stop_scanner();
        cleanup_test_account(&self.name);
        drop(self.bbd);
    }
}

#[test]
fn test_validate_unsigned_psbt_success() {
    let funded = funded();
    let psbt = funded.prepared(funded.account.sp_address(), "test psbt v2 validation");

    let validation = funded.account.validate_unsigned_psbt(&psbt);
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

    funded.finish();
}

#[test]
fn test_validate_unsigned_psbt_detects_reused_output() {
    let mut funded = funded();
    let bbd_url = funded.bbd.url();
    let bitcoind = &mut funded.node.client;

    // Pay this address once from outside the wallet, so its history is
    // already non-empty by the time the wallet targets it too.
    let reused_address = bitcoind.new_address().expect("new address");
    bwk_utils::test::send(bitcoind, reused_address.clone(), 0.01).expect("fund reused address");
    bwk_utils::test::generate_blocks(bitcoind, 2);
    let height = bwk_utils::test::get_height(bitcoind);
    wait_for_sync_and_index(&bbd_url, height as u32);

    let psbt = funded.prepared(reused_address.to_string(), "test psbt v2 reused output");

    let validation = funded.account.validate_unsigned_psbt(&psbt);
    assert!(
        validation.is_ok,
        "Validation should succeed: {}",
        validation.error
    );
    assert!(!validation.is_valid, "Validation should flag the reuse");
    assert_eq!(validation.reused_output_count, 1);
    assert_eq!(validation.spent_input_count, 0);

    funded.finish();
}

#[test]
fn test_prepared_psbt_signs_with_the_hot_key_and_finalizes() {
    let funded = funded();
    let psbt = funded.prepared(funded.account.sp_address(), "test psbt v2 signing");

    // Signed as the keys module signs for Send: the hot key of the test mnemonic, which
    // holds the account's descriptor.
    let mut keys = silent::new_keys_module(Network::Regtest);
    assert!(keys.is_ok(), "open the keys module: {}", keys.get_error());
    let sealed = silent::hot_seal(TEST_MNEMONIC, KEY_PASSWORD, Network::Regtest);
    assert!(sealed.is_ok(), "seal the hot key: {}", sealed.get_error());
    let committed = keys.commit_hot_key(&sealed, "sp");
    assert!(committed.is_ok, "commit the hot key: {}", committed.error);
    let registered = keys.register_descriptor(
        TEST_FINGERPRINT_HEX,
        &descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
    );
    assert!(
        registered.is_ok,
        "register the descriptor: {}",
        registered.error
    );
    let material = keys.hot_material(TEST_FINGERPRINT_HEX);
    assert!(material.is_ok(), "hot material: {}", material.get_error());
    let signed = silent::hot_sign(&material, KEY_PASSWORD, &hex::encode(&psbt));
    assert!(
        signed.is_ok,
        "the hot key signs the prepared PSBT: {}",
        signed.error
    );

    let signed = hex::decode(&signed.psbt_v2_hex).expect("signed PSBT hex");
    let finalized = funded.account.finalize_psbt(STANDARD.encode(signed));
    assert!(
        finalized.is_ok,
        "the account finalizes the signed PSBT: {}",
        finalized.error
    );
    let tx: Transaction =
        deserialize(&hex::decode(&finalized.value).expect("transaction hex")).expect("transaction");
    let spent: Vec<_> = PsbtV2::deserialize(&psbt)
        .expect("prepared PSBTv2")
        .inputs
        .iter()
        .map(|input| input.previous_output.to_string())
        .collect();
    assert_eq!(
        tx.input
            .iter()
            .map(|input| input.previous_output.to_string())
            .collect::<Vec<_>>(),
        spent,
        "the transaction spends the prepared inputs"
    );
    assert!(
        tx.input.iter().all(|input| !input.witness.is_empty()),
        "every input is signed"
    );

    funded.finish();
}
