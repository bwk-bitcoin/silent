//! PSBT export as base64 and import back, for Multikey saved PSBTs and SP Send.

mod common;

use std::str::FromStr;

use base64::{engine::general_purpose::STANDARD, Engine};
use bitcoin::{
    absolute::LockTime,
    bip32::{ChainCode, ChildNumber, DerivationPath, Xpriv, Xpub},
    hashes::Hash,
    secp256k1::Secp256k1,
    transaction::Version,
    Amount, NetworkKind, OutPoint, Transaction, TxIn, TxOut, Txid,
};
use bwk::{
    account::Account as BwkAccount, bwk_descriptor::descriptor::Descriptor,
    bwk_keys::nums::bip341_nums, bwk_sign::hot_signer::HotSigner, bwk_tx::bwk_psbt::PsbtV2,
};
use common::{
    cleanup_test_account, create_test_account_with_electrum,
    create_test_account_with_electrum_mnemonic, fund_sp_wallet, setup_blindbitd_with_electrum,
    test_account_name, wait_for_scan_complete, wait_for_sync_and_index, TEST_MNEMONIC,
};
use silent::{Config, MusigAccount, Network, Output, TransactionTemplate};

const FUNDING: u64 = 100_000;
const OTHER_MNEMONIC: &str =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";

#[derive(Debug, Clone, Copy)]
enum Policy {
    /// A NUMS internal key and one multi_a leaf: a leaf spend path.
    MultiA,
    /// A musig() internal key and no leaf: the key path.
    Musig,
}

/// The descriptor of `policy` over the keys of seeds `base..`, and their signers.
fn policy_keys(policy: Policy, base: u8) -> (String, Vec<HotSigner>) {
    let secp = Secp256k1::new();
    let count = match policy {
        Policy::MultiA => 3,
        Policy::Musig => 2,
    };
    let path = DerivationPath::from_str("m/87'/1'/0'/2").expect("role path");
    let mut keys = Vec::new();
    let mut signers = Vec::new();
    for seed in base..base + count {
        let master = Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).expect("master");
        let role = master.derive_priv(&secp, &path).expect("role xpriv");
        let fingerprint = master.fingerprint(&secp);
        keys.push(format!(
            "[{fingerprint}/87'/1'/0'/2]{}/<0;1>/*",
            Xpub::from_priv(&secp, &role)
        ));
        signers.push(HotSigner::new_from_xpriv(bitcoin::Network::Regtest, master));
    }
    let descriptor = match policy {
        Policy::MultiA => {
            let nums = Xpub {
                network: NetworkKind::Test,
                depth: 0,
                parent_fingerprint: Default::default(),
                child_number: ChildNumber::from_normal_idx(0).expect("normal child"),
                public_key: bip341_nums(),
                chain_code: ChainCode::from([7; 32]),
            };
            format!("tr({nums}/<0;1>/*,multi_a(2,{}))", keys.join(","))
        }
        Policy::Musig => {
            let participants: Vec<_> = keys
                .iter()
                .map(|key| key.strip_suffix("/<0;1>/*").expect("key suffix"))
                .collect();
            format!("tr(musig({})/<0;1>/*)", participants.join(","))
        }
    };
    (descriptor, signers)
}

/// A Multikey account of `descriptor` on `network`, holding the coin of `funding_seed`
/// (FUNDING sats on its first receive address) unless `funding_seed` is None.
struct Wallet {
    _config: Box<Config>,
    name: String,
    account: Box<MusigAccount>,
    /// The coin it holds, empty for none.
    outpoint: String,
}

impl Wallet {
    fn open(descriptor: &str, network: Network, funding_seed: Option<u8>) -> Self {
        let name = test_account_name();
        let config = silent::new_config(
            name.clone(),
            network,
            descriptor.into(),
            String::new(),
            String::new(),
            0,
            "musig".into(),
        );
        assert!(config.save().is_empty(), "save the {name} config");
        let mut outpoint = String::new();
        if let Some(seed) = funding_seed {
            let mut backend: BwkAccount =
                BwkAccount::try_new(config.to_musig_config().expect("backend config"))
                    .expect("backend");
            let address = backend.scanner_mut().new_recv_addr();
            let funding = funding(seed, &address);
            backend.scanner().record_unconfirmed_spend(&funding);
            outpoint = OutPoint::new(funding.compute_txid(), 0).to_string();
        }
        let account = silent::new_musig_account(name.clone());
        assert!(account.is_ok(), "open {name}: {}", account.get_error());
        Self {
            _config: config,
            name,
            account,
            outpoint,
        }
    }

    fn entries(&self) -> Vec<silent::SavedPsbtEntry> {
        let list = self.account.saved_psbts(true);
        assert!(list.is_ok, "list {}: {}", self.name, list.error);
        list.entries
    }

    fn close(self) {
        drop(self.account);
        cleanup_test_account(&self.name);
    }
}

fn funding(seed: u8, address: &bitcoin::Address) -> Transaction {
    Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::new(Txid::from_byte_array([seed; 32]), 0),
            ..Default::default()
        }],
        output: vec![TxOut {
            value: Amount::from_sat(FUNDING),
            script_pubkey: address.script_pubkey(),
        }],
    }
}

/// `psbt_v2_hex` with `signer`'s contribution: a signature, or a MuSig2 nonce.
fn sign(signer: &HotSigner, descriptor: &str, psbt_v2_hex: &str) -> String {
    let descriptor = Descriptor::from_str(descriptor).expect("descriptor");
    let mut psbt = PsbtV2::deserialize(&hex::decode(psbt_v2_hex).expect("PSBT hex")).expect("PSBT");
    signer
        .sign_v2(
            &mut psbt,
            descriptor.as_miniscript().expect("wallet descriptor"),
        )
        .expect("software signature or nonce");
    hex::encode(psbt.serialize().expect("serialize signed PSBT"))
}

fn base64_of(psbt_v2_hex: &str) -> String {
    let export = silent::export_psbt(&hex::decode(psbt_v2_hex).expect("PSBT hex"));
    assert!(export.is_ok, "export: {}", export.error);
    export.base64
}

// ===== Multikey Tests =====

/// The saved PSBT a cosigner exports is imported by another cosigner holding the same
/// coin, comes back to the first one signed, and is merged into its saved PSBT.
fn round_trip(policy: Policy, base: u8) {
    let (descriptor, signers) = policy_keys(policy, base);
    let first = Wallet::open(&descriptor, Network::Regtest, Some(base));
    let outpoint = first.outpoint.clone();
    let paths = first.account.spend_leaves(vec![outpoint.clone()]);
    assert!(paths.is_ok, "paths of {policy:?}: {}", paths.error);
    assert_eq!(paths.leaves.len(), 1, "one spend path for {policy:?}");
    let recipient = "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080".to_string();
    let prepared = common::musig_prepared(
        &first.account,
        TransactionTemplate {
            outputs: vec![Output {
                address: recipient.clone(),
                amount: 40_000,
                label: String::new(),
                max: false,
            }],
            fee: 1000,
            fee_rate: 0.0,
            input_outpoints: vec![outpoint.clone()],
        },
        paths.leaves[0].clone(),
    );
    assert!(
        prepared.is_ok(),
        "prepare {policy:?}: {}",
        prepared.get_psbt_error()
    );
    let saved = first.entries();
    assert_eq!(saved.len(), 1, "the prepared PSBT is saved");
    let exported = silent::export_psbt(
        &hex::decode(&saved[0].record.current_psbt_v2_hex).expect("saved PSBT hex"),
    );
    assert!(exported.is_ok, "export {policy:?}: {}", exported.error);
    assert_eq!(
        STANDARD.decode(&exported.base64).expect("exported base64"),
        hex::decode(prepared.bytes_v2_hex()).expect("prepared hex"),
        "the export is the base64 of the PSBTv2 the app signs"
    );
    assert_eq!(
        exported.txid,
        prepared.get_txid_preview(),
        "the export names its txid"
    );

    // The other cosigner, as base64 text with surrounding whitespace.
    let second = Wallet::open(&descriptor, Network::Regtest, Some(base));
    let imported = second
        .account
        .import_psbt(format!("  {}\n", exported.base64).as_bytes());
    assert!(
        imported.is_ok && !imported.merged,
        "import {policy:?}: {}",
        imported.error
    );
    let entries = second.entries();
    assert_eq!(entries.len(), 1, "the imported PSBT is saved once");
    let entry = &entries[0];
    assert_eq!(entry.record.id, imported.record_id, "the saved record id");
    assert_eq!(
        entry.record.current_psbt_v2_hex,
        prepared.bytes_v2_hex(),
        "saved as imported"
    );
    assert_eq!(entry.txid, exported.txid, "the same transaction");
    assert_eq!(
        entry.record.spend.leaf_hash, paths.leaves[0].leaf_hash,
        "the spend path it is restricted to"
    );
    assert_eq!(
        entry.record.spend.key_path, paths.leaves[0].key_path,
        "the spend path kind"
    );
    assert_eq!(
        entry.frozen_template.outputs.len(),
        1,
        "the change is no recipient"
    );
    assert_eq!(entry.frozen_template.outputs[0].address, recipient);
    assert_eq!(entry.frozen_template.outputs[0].amount, 40_000);
    assert_eq!(entry.frozen_template.fee, 1000, "the fee it pays");
    assert_eq!(entry.frozen_template.input_outpoints, vec![outpoint]);
    let restored = second.account.activate_saved_psbt(&imported.record_id);
    assert!(
        restored.is_ok,
        "the import is ready to sign: {}",
        restored.error
    );
    assert!(
        second.account.clear_active_attempt(),
        "close the other cosigner's attempt"
    );

    // Signed by the other cosigner, then imported back as a binary PSBT.
    let signed = sign(&signers[0], &descriptor, &prepared.bytes_v2_hex());
    let back = first
        .account
        .import_psbt(&hex::decode(&signed).expect("signed hex"));
    assert!(
        back.is_ok && back.merged,
        "merge {policy:?}: {}",
        back.error
    );
    assert_eq!(back.record_id, saved[0].record.id, "into the saved PSBT");
    let merged = first.entries();
    assert_eq!(merged.len(), 1, "no second entry for the same transaction");
    assert_ne!(
        merged[0].record.current_psbt_v2_hex,
        prepared.bytes_v2_hex(),
        "the contribution is merged"
    );
    assert_eq!(
        first.account.active_attempt().psbt_v2_hex,
        merged[0].record.current_psbt_v2_hex,
        "the active attempt has it too"
    );
    match policy {
        Policy::MultiA => assert_eq!(merged[0].signatures_complete, 1, "one signature in"),
        Policy::Musig => {
            let status = first
                .account
                .round_status(&merged[0].record.current_psbt_v2_hex);
            assert!(
                status.cosigners.iter().any(|cosigner| cosigner.nonces > 0),
                "the nonce is in: {status:?}"
            );
        }
    }

    // The same again, into a saved PSBT that is not the active attempt.
    let again = second
        .account
        .import_psbt(base64_of(&merged[0].record.current_psbt_v2_hex).as_bytes());
    assert!(
        again.is_ok && again.merged,
        "merge into the inactive saved PSBT: {}",
        again.error
    );
    let entries = second.entries();
    assert_eq!(entries.len(), 1, "still one entry");
    assert_eq!(
        entries[0].record.current_psbt_v2_hex, merged[0].record.current_psbt_v2_hex,
        "the inactive saved PSBT has the contribution"
    );
    first.close();
    second.close();
}

#[test]
fn multi_a_saved_psbt_round_trips_between_cosigners() {
    round_trip(Policy::MultiA, 61);
}

#[test]
fn musig_saved_psbt_round_trips_between_cosigners() {
    round_trip(Policy::Musig, 71);
}

#[test]
fn foreign_invalid_and_wrong_network_psbts_are_refused() {
    let (descriptor, _) = policy_keys(Policy::Musig, 81);
    let origin = Wallet::open(&descriptor, Network::Regtest, Some(81));
    let outpoint = origin.outpoint.clone();
    let paths = origin.account.spend_leaves(vec![outpoint.clone()]);
    assert!(paths.is_ok, "paths: {}", paths.error);
    let prepared = common::musig_prepared(
        &origin.account,
        TransactionTemplate {
            outputs: vec![Output {
                address: "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080".into(),
                amount: 40_000,
                label: String::new(),
                max: false,
            }],
            fee: 1000,
            fee_rate: 0.0,
            input_outpoints: vec![outpoint],
        },
        paths.leaves[0].clone(),
    );
    assert!(prepared.is_ok(), "prepare: {}", prepared.get_psbt_error());
    let text = base64_of(&prepared.bytes_v2_hex());

    let (foreign_descriptor, _) = policy_keys(Policy::Musig, 91);
    let foreign = Wallet::open(&foreign_descriptor, Network::Regtest, Some(91));
    let refused = foreign.account.import_psbt(text.as_bytes());
    assert!(!refused.is_ok, "a PSBT of another account is refused");
    assert!(
        refused
            .error
            .contains("The PSBT does not spend this account's coins"),
        "and says why: {}",
        refused.error
    );
    assert!(foreign.entries().is_empty(), "nothing is saved");

    // The same descriptor on another network holds none of the coins.
    let signet = Wallet::open(&descriptor, Network::Signet, None);
    let refused = signet.account.import_psbt(text.as_bytes());
    assert!(
        !refused.is_ok
            && refused
                .error
                .contains("The PSBT does not spend this account's coins"),
        "a PSBT of another network is refused: {}",
        refused.error
    );
    assert!(signet.entries().is_empty(), "nothing is saved on signet");

    for (garbage, why) in [
        (&b"not a psbt"[..], "is not valid base64"),
        (b"aGVsbG8gd29ybGQ=", "The text is not a PSBT"),
        (b"psbt\xff\x01\x02", "Invalid PSBT"),
    ] {
        let refused = origin.account.import_psbt(garbage);
        assert!(
            !refused.is_ok && refused.error.contains(why),
            "{garbage:?} is refused with '{why}': {}",
            refused.error
        );
    }
    assert_eq!(origin.entries().len(), 1, "only the prepared PSBT is saved");
    origin.close();
    foreign.close();
    signet.close();
}

// ===== SP Tests =====

#[test]
fn sp_send_psbt_exports_and_imports_back() {
    let account_name = test_account_name();
    let other_name = test_account_name();
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

    let psbt = common::sp_unsigned_psbt(
        &account,
        TransactionTemplate {
            outputs: vec![Output {
                address: account.sp_address(),
                amount: balance / 2,
                label: String::new(),
                max: false,
            }],
            fee_rate: 1.0,
            fee: 0,
            input_outpoints: vec![],
        },
    )
    .unwrap_or_else(|e| panic!("Prepare should succeed: {e}"));
    let exported = silent::export_psbt(&psbt);
    assert!(exported.is_ok, "export: {}", exported.error);
    assert_eq!(
        exported.base64,
        STANDARD.encode(&psbt),
        "the export is the base64 of the PSBTv2 the app signs"
    );

    let imported = account.import_psbt(exported.base64.as_bytes());
    assert!(imported.is_ok, "import: {}", imported.error);
    assert_eq!(imported.psbt, psbt, "the PSBT the app would sign");
    let template = &imported.tx_template;
    assert_eq!(template.outputs.len(), 1, "the SP change is no recipient");
    assert_eq!(template.outputs[0].address, account.sp_address());
    assert_eq!(template.outputs[0].amount, balance / 2);
    let parsed = PsbtV2::deserialize(&psbt).expect("PSBT");
    let input_total: u64 = account
        .coins()
        .iter()
        .filter(|coin| template.input_outpoints.contains(&coin.outpoint))
        .map(|coin| coin.value)
        .sum();
    let output_total: u64 = parsed.outputs.iter().map(|o| o.amount.to_sat()).sum();
    assert_eq!(
        template.fee,
        input_total - output_total,
        "the absolute fee it pays"
    );
    assert_eq!(
        template.input_outpoints,
        parsed
            .inputs
            .iter()
            .map(|input| input.previous_output.to_string())
            .collect::<Vec<_>>(),
        "its inputs"
    );

    let binary = account.import_psbt(&psbt);
    assert!(
        binary.is_ok && binary.psbt == psbt,
        "a binary PSBT is accepted too: {}",
        binary.error
    );

    let other = create_test_account_with_electrum_mnemonic(
        &other_name,
        &bbd_url,
        &electrum_url,
        OTHER_MNEMONIC,
    );
    let refused = other.import_psbt(exported.base64.as_bytes());
    assert!(
        !refused.is_ok && refused.error.contains("a coin this account does not hold"),
        "a PSBT of another account is refused: {}",
        refused.error
    );
    let refused = account.import_psbt(b"not a psbt");
    assert!(!refused.is_ok, "garbage is refused");

    drop(other);
    drop(account);
    cleanup_test_account(&other_name);
    cleanup_test_account(&account_name);
}
