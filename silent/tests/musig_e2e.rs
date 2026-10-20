//! MuSig2 end to end on regtest through the MusigAccount binding: a 2-of-3 account
//! (bitcoind and electrs), funded, spent through the leaf of A and B in two rounds by
//! bwk hot signers, finalized, broadcast and confirmed; and the restart of round 1
//! after a signer lost its session.
//!
//! Run with `cargo test --test musig_e2e -- --ignored --test-threads=1`, under a
//! throwaway `HOME`: the accounts live in `~/.silent`.

mod common;

use std::{
    str::FromStr,
    thread,
    time::{Duration, Instant},
};

use bitcoin::bip32::DerivationPath;
use bwk_sp::{
    bwk_sign::{
        bwk_descriptor::descriptor::Descriptor, error::Error as SignError, hot_signer::HotSigner,
    },
    bwk_tx::bwk_psbt::PsbtV2,
};
use bwk_utils::test as bwk_test;
use silent::{
    compile_policy, musig_session_restart, new_config, new_musig_account, MusigAccount, MusigRound,
    Network, Output, PolicyKey, PolicyKeyType, PolicyLocktime, PolicyPath, PolicySemantic,
    PolicySpec, SavedPsbtRestore, TransactionTemplate, MUSIG_PLUGIN_ID,
};

use common::{cleanup_test_account, setup_blindbitd_with_electrum, test_account_name};

const MNEMONIC_A: &str =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
const MNEMONIC_B: &str =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";
const MNEMONIC_C: &str =
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above";
const ACCOUNT_PATH: &str = "m/87'/1'/0'";
const FUND_BTC: f64 = 0.01;
const SEND_SATS: u64 = 500_000;
const FEE_RATE: f64 = 2.0;
const TIMEOUT: Duration = Duration::from_secs(60);

// ===== Helpers =====

fn signer(mnemonic: &str) -> HotSigner {
    HotSigner::new_from_mnemonics(bitcoin::Network::Regtest, mnemonic).expect("valid signer")
}

fn policy_key(signer: &HotSigner, key_type: PolicyKeyType) -> PolicyKey {
    let path = DerivationPath::from_str(ACCOUNT_PATH).expect("account path");
    PolicyKey {
        fingerprint: signer.fingerprint().to_string(),
        account_path: ACCOUNT_PATH.to_string(),
        role_xpub: signer.xpub(&path).xkey.to_string(),
        key_type,
        mandatory: false,
    }
}

/// One Multi 2-of-3 path over A, B and C, typed Internal, External and Cosigner.
fn two_of_three(a: &HotSigner, b: &HotSigner, c: &HotSigner) -> String {
    let result = compile_policy(PolicySpec {
        network: Network::Regtest,
        paths: vec![PolicyPath {
            semantic: PolicySemantic::Multi,
            threshold: 2,
            locktime: PolicyLocktime::None,
            locktime_value: 0,
            keys: vec![
                policy_key(a, PolicyKeyType::Internal),
                policy_key(b, PolicyKeyType::External),
                policy_key(c, PolicyKeyType::Cosigner),
            ],
        }],
    });
    assert!(result.is_ok, "compile_policy: {}", result.error);
    assert!(
        result.descriptor.contains("musig("),
        "the 2-of-3 should compile to musig() leaves, got {}",
        result.descriptor
    );
    result.descriptor
}

/// A musig account named `account_name` over `descriptor`, listening to electrs.
fn open_account(account_name: &str, descriptor: String, electrum_url: &str) -> Box<MusigAccount> {
    new_config(
        account_name.to_string(),
        Network::Regtest,
        descriptor,
        String::new(),
        electrum_url.to_string(),
        0,
        MUSIG_PLUGIN_ID.to_string(),
    )
    .to_file();
    let mut account = new_musig_account(account_name.to_string());
    assert!(
        account.is_ok(),
        "account should open: {}",
        account.get_error()
    );
    assert!(account.start_electrum(), "electrum should start");
    account
}

/// What the waits look at in a coin.
#[derive(Debug)]
struct CoinView {
    outpoint: String,
    spent: bool,
    height: u32,
}

/// Polls the account until `done` holds on its coins, draining notifications.
fn wait_coins(account: &mut MusigAccount, what: &str, done: impl Fn(&[CoinView]) -> bool) {
    let start = Instant::now();
    loop {
        while account.try_recv().is_some() {}
        let coins: Vec<_> = account
            .coins()
            .iter()
            .map(|c| CoinView {
                outpoint: c.outpoint.clone(),
                spent: c.spent,
                height: c.height,
            })
            .collect();
        if done(&coins) {
            return;
        }
        assert!(
            start.elapsed() < TIMEOUT,
            "timed out waiting for {what}, coins {coins:?}"
        );
        thread::sleep(Duration::from_millis(300));
    }
}

/// The wallet descriptor a hot signer signs the account with.
fn wallet_descriptor(account: &MusigAccount) -> Descriptor {
    Descriptor::from_str(&account.descriptor()).expect("account descriptor parses")
}

/// One round by `signer` on the v2 hex PSBT.
fn sign_round(
    signer: &HotSigner,
    descriptor: &Descriptor,
    psbt_v2_hex: &str,
) -> Result<String, SignError> {
    let bytes = hex::decode(psbt_v2_hex).expect("psbt hex");
    let mut psbt = PsbtV2::deserialize(&bytes)
        .expect("psbt v2")
        .into_bitcoin_psbt()
        .expect("psbt v0");
    let miniscript = descriptor.as_miniscript().expect("miniscript descriptor");
    signer.inner_sign(&mut psbt, miniscript)?;
    let v2 = PsbtV2::from_bitcoin_psbt(psbt).expect("back to v2");
    Ok(hex::encode(v2.serialize().expect("serialize v2")))
}

fn assert_round(account: &MusigAccount, psbt_v2_hex: &str, expected: MusigRound) {
    let status = account.round_status(psbt_v2_hex);
    assert!(status.is_ok, "round_status: {}", status.error);
    assert_eq!(
        status.round,
        expected,
        "round, cosigners {:?}",
        status
            .cosigners
            .iter()
            .map(|c| (c.fingerprint.clone(), c.nonces, c.partial_sigs, c.expected))
            .collect::<Vec<_>>()
    );
}

/// A funded account and its prepared PSBT, restricted to the leaf of A and B.
struct Spend {
    account: Option<Box<MusigAccount>>,
    account_name: String,
    descriptor: Descriptor,
    prepared: String,
    saved_record_id: String,
    a: HotSigner,
    b: HotSigner,
}

impl Spend {
    fn account(&self) -> &MusigAccount {
        self.account.as_ref().expect("open test account")
    }

    fn account_mut(&mut self) -> &mut MusigAccount {
        self.account.as_mut().expect("open test account")
    }

    fn reload(&mut self) -> SavedPsbtRestore {
        let mut previous = self.account.take().expect("account before restart");
        previous.stop_electrum();
        drop(previous);
        let mut account = new_musig_account(self.account_name.clone());
        assert!(account.is_ok(), "reopen account: {}", account.get_error());
        let restored = account.restore_saved_psbt(&self.saved_record_id);
        assert!(restored.is_ok, "restore saved PSBT: {}", restored.error);
        assert!(
            restored.save_error.is_empty(),
            "persisted progress: {}",
            restored.save_error
        );
        assert!(
            account.start_electrum(),
            "restart isolated regtest listener"
        );
        self.account = Some(account);
        restored
    }
}

fn fund_and_prepare(bitcoind: &mut bwk_test::corepc_node::Client, electrum_url: &str) -> Spend {
    let (a, b, c) = (signer(MNEMONIC_A), signer(MNEMONIC_B), signer(MNEMONIC_C));
    let account_name = test_account_name();
    let mut account = open_account(&account_name, two_of_three(&a, &b, &c), electrum_url);
    let descriptor = wallet_descriptor(&account);

    let address = bitcoin::Address::from_str(&account.new_addr())
        .expect("address")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest address");
    let fund_txid = bwk_test::send(bitcoind, address, FUND_BTC).expect("fund the account");
    bwk_test::generate_blocks(bitcoind, 1);
    let funded = format!("{fund_txid}:");
    wait_coins(&mut account, "the funding coin", |coins| {
        coins
            .iter()
            .any(|c| c.outpoint.starts_with(&funded) && !c.spent && c.height > 0)
    });
    let outpoint = account
        .coins()
        .iter()
        .find(|c| c.outpoint.starts_with(&funded))
        .expect("funding coin")
        .outpoint
        .clone();

    let leaves = account.spend_leaves(vec![outpoint.clone()]);
    assert!(leaves.is_ok, "spend_leaves: {}", leaves.error);
    assert!(
        leaves.leaves.iter().all(|leaf| !leaf.key_path),
        "a 2-of-3 has no key path spend"
    );
    let mut ab = vec![a.fingerprint().to_string(), b.fingerprint().to_string()];
    ab.sort();
    let leaf = leaves
        .leaves
        .iter()
        .find(|leaf| {
            let mut signers = leaf.fingerprints.clone();
            signers.sort();
            signers == ab
        })
        .expect("a leaf signed by A and B")
        .clone();
    assert!(leaf.spendable, "the A and B leaf has no timelock");

    let to = bitcoind.new_address().expect("miner address");
    let psbt = account.prepare_transaction(
        TransactionTemplate {
            outputs: vec![Output {
                address: to.to_string(),
                amount: SEND_SATS,
                label: String::new(),
                max: false,
            }],
            fee_rate: FEE_RATE,
            fee: 0,
            input_outpoints: vec![outpoint],
        },
        leaf,
    );
    assert!(
        psbt.is_ok(),
        "prepare_transaction: {}",
        psbt.get_psbt_error()
    );
    let prepared = psbt.bytes_v2_hex();
    assert!(
        psbt.save_error().is_empty(),
        "persist prepared PSBT: {}",
        psbt.save_error()
    );
    let saved_record_id = psbt.saved_record_id();
    assert!(
        !saved_record_id.is_empty(),
        "preparation creates a saved record"
    );
    assert_round(&account, &prepared, MusigRound::Nonces);
    let mut spend = Spend {
        account: Some(account),
        account_name,
        descriptor,
        prepared,
        saved_record_id,
        a,
        b,
    };
    let restored = spend.reload();
    assert_eq!(
        restored.entry.record.current_psbt_v2_hex, spend.prepared,
        "prepared PSBT survives account restart"
    );
    spend
}

/// Both rounds by A then B from `psbt`, checking the round after each.
fn both_rounds(spend: &mut Spend, psbt: &str) -> String {
    let psbt = sign_round(&spend.a, &spend.descriptor, psbt).expect("A round 1");
    assert_round(spend.account(), &psbt, MusigRound::Nonces);
    let psbt = persist_and_reload(spend, &psbt);
    let psbt = sign_round(&spend.b, &spend.descriptor, &psbt).expect("B round 1");
    assert_round(spend.account(), &psbt, MusigRound::PartialSigs);
    let psbt = persist_and_reload(spend, &psbt);
    let psbt = sign_round(&spend.a, &spend.descriptor, &psbt).expect("A round 2");
    assert_round(spend.account(), &psbt, MusigRound::PartialSigs);
    let psbt = persist_and_reload(spend, &psbt);
    assert_eq!(
        spend
            .account()
            .round_status(&psbt)
            .cosigners
            .iter()
            .map(|signer| signer.partial_sigs)
            .sum::<u32>(),
        1,
        "reloaded PSBT contains A's actual partial signature before B signs"
    );
    let psbt = sign_round(&spend.b, &spend.descriptor, &psbt).expect("B round 2");
    assert_round(spend.account(), &psbt, MusigRound::Ready);
    persist_and_reload(spend, &psbt)
}

fn persist_and_reload(spend: &mut Spend, psbt: &str) -> String {
    let saved = spend
        .account()
        .update_saved_psbt(&spend.saved_record_id, psbt);
    assert!(
        saved.is_ok && saved.save_error.is_empty(),
        "persist signing stage: {} {}",
        saved.error,
        saved.save_error
    );
    let expected = spend.account().round_status(psbt).round;
    let restored = spend.reload();
    assert!(restored.is_ok, "restore signing stage: {}", restored.error);
    assert_eq!(
        restored.entry.record.current_psbt_v2_hex, psbt,
        "every signing stage preserves exact PSBT bytes"
    );
    assert_eq!(
        restored.entry.record.original_psbt_v2_hex, spend.prepared,
        "signing progress never replaces original prepared PSBT"
    );
    assert_eq!(
        restored.entry.frozen_template.outputs[0].amount, SEND_SATS,
        "restored recipient stays frozen"
    );
    assert_round(
        spend.account(),
        &restored.entry.record.current_psbt_v2_hex,
        expected,
    );
    restored.entry.record.current_psbt_v2_hex
}

/// Finalizes, broadcasts through electrs, mines and checks the spend confirmed.
fn finish(spend: &mut Spend, bitcoind: &mut bwk_test::corepc_node::Client, psbt: &str) {
    let tx = spend.account().finalize_psbt(psbt);
    assert!(tx.is_ok, "finalize_psbt: {}", tx.error);
    assert!(
        tx.save_error.is_empty(),
        "persist finalization: {}",
        tx.save_error
    );
    let restored = spend.reload();
    assert!(
        restored.is_ok,
        "restore finalized transaction: {}",
        restored.error
    );
    assert_eq!(
        restored.entry.record.current_psbt_v2_hex, tx.psbt_v2_hex,
        "finalized PSBT survives restart"
    );
    assert_eq!(
        restored.entry.record.finalized_tx_hex, tx.tx_hex,
        "ready-to-broadcast transaction survives restart"
    );
    let sent = spend
        .account()
        .broadcast_transaction(restored.entry.record.finalized_tx_hex);
    assert!(sent.is_ok, "broadcast_transaction: {}", sent.error);
    let txid = bitcoin::Txid::from_str(&sent.value).expect("txid");
    bwk_test::generate_blocks(bitcoind, 1);
    let confirmations = bitcoind
        .get_raw_transaction_verbose(txid)
        .expect("spend known to bitcoind")
        .confirmations
        .unwrap_or(0);
    assert!(confirmations >= 1, "spend {txid} should be confirmed");
    eprintln!("spend {txid} confirmed ({confirmations} confirmation)");
    wait_coins(spend.account_mut(), "the coin spent", |coins| {
        coins.iter().any(|c| c.spent)
    });
    let start = Instant::now();
    loop {
        let restored = spend.account().restore_saved_psbt(&spend.saved_record_id);
        assert!(
            restored.is_ok,
            "confirmed record is retained: {}",
            restored.error
        );
        if restored.entry.confirmed_height > 0 {
            break;
        }
        assert!(
            start.elapsed() < TIMEOUT,
            "timed out waiting for saved transaction confirmation"
        );
        thread::sleep(Duration::from_millis(300));
    }
    let visible = spend.account().saved_psbts(false);
    assert!(visible.is_ok, "default saved list: {}", visible.error);
    assert!(
        !visible
            .entries
            .iter()
            .any(|entry| entry.record.id == spend.saved_record_id),
        "confirmed records are hidden by default"
    );
    let all = spend.account().saved_psbts(true);
    assert!(all.is_ok, "full saved list: {}", all.error);
    assert!(
        all.entries
            .iter()
            .any(|entry| entry.record.id == spend.saved_record_id && entry.record.broadcast),
        "confirmed broadcast records remain available with toggle enabled"
    );
}

// ===== Tests =====

#[test]
#[ignore = "needs bitcoind and electrs"]
fn musig_2_of_3_spends_through_a_2_subset_leaf() {
    let (bbd, mut node, electrum_url) = setup_blindbitd_with_electrum();
    let bitcoind = &mut node.client;
    bwk_test::generate_blocks(bitcoind, 101);

    let mut spend = fund_and_prepare(bitcoind, &electrum_url);
    let prepared = spend.prepared.clone();
    let signed = both_rounds(&mut spend, &prepared);
    finish(&mut spend, bitcoind, &signed);

    cleanup_test_account(&spend.account_name);
    drop(bbd);
}

#[test]
#[ignore = "needs bitcoind and electrs"]
fn musig_lost_session_restarts_round_one() {
    let (bbd, mut node, electrum_url) = setup_blindbitd_with_electrum();
    let bitcoind = &mut node.client;
    bwk_test::generate_blocks(bitcoind, 101);

    let mut spend = fund_and_prepare(bitcoind, &electrum_url);
    let psbt = sign_round(&spend.a, &spend.descriptor, &spend.prepared).expect("A round 1");
    let psbt = sign_round(&spend.b, &spend.descriptor, &psbt).expect("B round 1");
    assert_round(spend.account(), &psbt, MusigRound::PartialSigs);

    let psbt = persist_and_reload(&mut spend, &psbt);

    // B restarts: a fresh signer holds no session for round 1's nonces.
    spend.b = signer(MNEMONIC_B);
    let lost = sign_round(&spend.b, &spend.descriptor, &psbt);
    assert!(
        matches!(lost, Err(SignError::MusigSession)),
        "B round 2 without its session should fail on the session, got {lost:?}"
    );

    let restarted = musig_session_restart(
        Network::Regtest,
        &spend.descriptor.to_string(),
        &psbt,
        &spend.prepared,
    );
    assert!(
        restarted.is_ok,
        "explicit signing restart: {}",
        restarted.error
    );
    assert_ne!(
        restarted.psbt_v2_hex, spend.prepared,
        "restart creates a fresh signing attempt for the same transaction"
    );
    let prepared = persist_and_reload(&mut spend, &restarted.psbt_v2_hex);
    assert_round(spend.account(), &prepared, MusigRound::Nonces);
    let signed = both_rounds(&mut spend, &prepared);
    finish(&mut spend, bitcoind, &signed);

    cleanup_test_account(&spend.account_name);
    drop(bbd);
}
