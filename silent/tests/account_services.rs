//! Rust rules behind the account UI services, over real isolated Regtest stores:
//! crafting and finalizing without selection or derivation, independent templates, change
//! adoption, a restore bringing back no saved PSBT, labels kept in RAM after a failed save,
//! broadcast outcomes and endpoint settings.

mod common;

use std::{
    io::{BufRead, BufReader, Write},
    net::TcpListener,
    str::FromStr,
    thread,
};

use bitcoin::{
    absolute::LockTime,
    bip32::{ChainCode, ChildNumber, DerivationPath, Xpriv, Xpub},
    consensus::encode::serialize,
    hashes::Hash,
    secp256k1::Secp256k1,
    taproot::TapLeafHash,
    transaction::Version,
    Amount, NetworkKind, OutPoint, ScriptBuf, Transaction, TxIn, TxOut, Txid,
};
use bwk::{account::Account as BwkAccount, bwk_keys::nums::bip341_nums, bwk_tx::bwk_psbt::PsbtV2};
use silent::{
    config_from_file, new_account, new_config, new_musig_account, new_musig_account_offline,
    restore_account_backup, save_endpoints, test_endpoint, BackendErrorKind, BackendServer,
    BroadcastOutcome, Config, EndpointSetting, LabelKind, MusigAccount, Network, Output,
    TransactionTemplate, MUSIG_PLUGIN_ID,
};

use common::{cleanup_test_account, descriptor_from_mnemonic, test_account_name, TEST_MNEMONIC};

const FUNDING: u64 = 100_000;
const SENT: u64 = 40_000;
const FEE: u64 = 1000;
const CHANGE: u64 = FUNDING - SENT - FEE;

fn role_keys() -> Vec<String> {
    let secp = Secp256k1::new();
    let keys: Vec<_> = [1, 2, 3]
        .into_iter()
        .map(|seed| {
            let master =
                Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).expect("master key");
            let role = master
                .derive_priv(
                    &secp,
                    &DerivationPath::from_str("m/87'/1'/0'/2").expect("role path"),
                )
                .expect("role key");
            format!(
                "[{}/87'/1'/0'/2]{}",
                master.fingerprint(&secp),
                Xpub::from_priv(&secp, &role)
            )
        })
        .collect();
    keys
}

fn musig_descriptor() -> String {
    let keys = role_keys();
    format!(
        "tr(musig({},{})/<0;1>/*,pk({}/<2;3>/*))",
        keys[0], keys[1], keys[2]
    )
}

/// The MuSig key path and two leaves: restricted to one leaf, a PSBT drops the other.
fn two_leaf_descriptor() -> String {
    let keys = role_keys();
    format!(
        "tr(musig({},{})/<0;1>/*,{{pk({}/<2;3>/*),pk({}/<4;5>/*)}})",
        keys[0], keys[1], keys[2], keys[2]
    )
}

/// A key path no one can sign for, and one leaf locked for ten blocks.
fn timelocked_descriptor() -> String {
    let nums = Xpub {
        network: NetworkKind::Test,
        depth: 0,
        parent_fingerprint: Default::default(),
        child_number: ChildNumber::from_normal_idx(0).expect("normal child"),
        public_key: bip341_nums(),
        chain_code: ChainCode::from([7; 32]),
    };
    format!(
        "tr({nums}/<0;1>/*,and_v(v:pk({}/<0;1>/*),older(10)))",
        role_keys()[0]
    )
}

/// A saved Multikey config on `electrum_url` whose wallet holds one unconfirmed coin of
/// FUNDING sats.
fn funded_multikey(electrum_url: &str) -> (Box<Config>, String) {
    funded_with(musig_descriptor(), electrum_url)
}

fn funded_with(descriptor: String, electrum_url: &str) -> (Box<Config>, String) {
    let name = test_account_name();
    let mut config = new_config(
        name,
        Network::Regtest,
        descriptor,
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save isolated Multikey account");
    let mut bwk: BwkAccount =
        BwkAccount::try_new(config.to_musig_config().expect("backend config")).expect("backend");
    let receive = bwk.scanner_mut().new_recv_addr();
    let funding = Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::new(Txid::from_byte_array([77; 32]), 0),
            ..Default::default()
        }],
        output: vec![TxOut {
            value: Amount::from_sat(FUNDING),
            script_pubkey: receive.script_pubkey(),
        }],
    };
    bwk.scanner().record_unconfirmed_spend(&funding);
    drop(bwk);
    // Funded offline: the fixture server answers the account alone.
    config.set_electrum_url(electrum_url.into());
    assert!(config.save().is_empty(), "save the Electrum endpoint");
    let outpoint = OutPoint::new(funding.compute_txid(), 0).to_string();
    (config, outpoint)
}

fn sp_config() -> Box<Config> {
    let config = new_config(
        test_account_name(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://127.0.0.1:1".into(),
        String::new(),
        0,
        "sp".into(),
    );
    assert!(config.save().is_empty(), "save isolated SP account");
    config
}

fn template(recipient: String, inputs: Vec<String>) -> TransactionTemplate {
    TransactionTemplate {
        outputs: vec![Output {
            address: recipient,
            amount: SENT,
            label: String::new(),
            max: false,
        }],
        fee: FEE,
        fee_rate: 0.0,
        input_outpoints: inputs,
    }
}

fn recipient() -> String {
    bitcoin::Address::p2wsh(&ScriptBuf::new(), bitcoin::Network::Regtest).to_string()
}

/// The script the change output of an unsigned PSBT pays to.
fn change_script(psbt: &[u8]) -> ScriptBuf {
    PsbtV2::deserialize(psbt)
        .expect("PSBTv2")
        .into_bitcoin_psbt()
        .expect("bitcoin PSBT")
        .unsigned_tx
        .output
        .into_iter()
        .find(|txout| txout.value == Amount::from_sat(CHANGE))
        .expect("change output")
        .script_pubkey
}

fn finalized(account: &MusigAccount, tx: &TransactionTemplate) -> Vec<u8> {
    let psbt = account.finalize_transaction(tx.clone());
    assert!(psbt.is_ok, "finalize: {}", psbt.error);
    psbt.psbt
}

/// What an unsigned PSBT pays: each output's script and amount, sorted.
fn payments(psbt: &[u8]) -> Vec<(ScriptBuf, u64)> {
    let mut outputs: Vec<_> = PsbtV2::deserialize(psbt)
        .expect("PSBTv2")
        .into_bitcoin_psbt()
        .expect("bitcoin PSBT")
        .unsigned_tx
        .output
        .into_iter()
        .map(|txout| (txout.script_pubkey, txout.value.to_sat()))
        .collect();
    outputs.sort();
    outputs
}

#[test]
fn multikey_crafting_selects_nothing_and_adoption_registers_change_once() {
    let (config, outpoint) = funded_multikey("");
    let account = new_musig_account(config.account_name.clone());
    assert!(account.is_ok(), "open Multikey: {}", account.get_error());

    let empty = template(recipient(), Vec::new());
    let crafted = account.craft_transaction(empty.clone());
    assert!(
        !crafted.complete && crafted.error.contains("No inputs"),
        "craft without inputs is incomplete: {}",
        crafted.error
    );
    assert!(
        crafted.transaction.input_outpoints.is_empty(),
        "craft selects no input"
    );
    let unsigned = account.finalize_transaction(empty.clone());
    assert!(
        !unsigned.is_ok && unsigned.psbt.is_empty() && unsigned.error.contains("No inputs"),
        "finalize without inputs builds nothing: {}",
        unsigned.error
    );

    let selected = account.select_transaction_coins(empty);
    assert!(selected.complete, "selection: {}", selected.error);
    assert_eq!(
        selected.transaction.input_outpoints,
        vec![outpoint.clone()],
        "explicit selection picks the funded coin"
    );
    let tx = selected.transaction;
    assert!(account.craft_transaction(tx.clone()).complete, "craft");

    let mut by_rate = tx.clone();
    by_rate.fee = 0;
    by_rate.fee_rate = 2.0;
    let simulated = account.simulate_transaction(by_rate.clone());
    let crafted = account.craft_transaction(by_rate);
    assert!(
        crafted.complete && simulated.is_valid,
        "fee rate craft: {}",
        crafted.error
    );
    assert!(
        crafted.transaction.fee > 0 && crafted.transaction.fee == simulated.fee,
        "craft states the fee its rate pays: {} for {}",
        crafted.transaction.fee,
        simulated.fee
    );

    let mut max = tx.clone();
    max.outputs[0].max = true;
    max.outputs[0].amount = 0;
    let max = account.craft_transaction(max);
    assert!(max.complete, "MAX craft: {}", max.error);
    assert_eq!(
        max.transaction.outputs[0].amount,
        FUNDING - FEE,
        "craft resolves the MAX amount"
    );

    let first = finalized(&account, &tx);
    let second = finalized(&account, &tx);
    let change = change_script(&first);
    assert_eq!(
        change_script(&second),
        change,
        "finalizing does not advance the change derivation"
    );

    let leaves = account.spend_leaves(vec![outpoint]);
    assert!(leaves.is_ok, "spend leaves: {}", leaves.error);
    let leaf = leaves.leaves[0].clone();
    let mut other = tx.clone();
    other.outputs[0].amount = SENT - 1;
    let mismatched = finalized(&account, &other);
    let refused = account.adopt_transaction(tx.clone(), leaf.clone(), &mismatched);
    assert!(
        !refused.is_ok() && refused.get_psbt_error().contains("does not list"),
        "a PSBT paying other amounts than its template is refused: {}",
        refused.get_psbt_error()
    );
    let mut more = tx.clone();
    more.outputs.push(more.outputs[0].clone());
    let refused = account.adopt_transaction(more, leaf.clone(), &first);
    assert!(
        !refused.is_ok() && refused.get_psbt_error().contains("every recipient"),
        "a PSBT missing a recipient of its template is refused: {}",
        refused.get_psbt_error()
    );
    let mut stripped = PsbtV2::deserialize(&first)
        .expect("PSBTv2")
        .into_bitcoin_psbt()
        .expect("bitcoin PSBT");
    let change_at = stripped
        .unsigned_tx
        .output
        .iter()
        .position(|txout| txout.value == Amount::from_sat(CHANGE))
        .expect("change output");
    assert!(
        stripped.outputs[change_at].tap_internal_key.is_some()
            && !stripped.outputs[change_at].tap_key_origins.is_empty(),
        "finalize writes the change derivation"
    );
    stripped.outputs[change_at].tap_key_origins.clear();
    let stripped = PsbtV2::from_bitcoin_psbt(stripped)
        .and_then(|v2| v2.serialize())
        .expect("PSBTv2 bytes");
    let refused = account.adopt_transaction(tx.clone(), leaf.clone(), &stripped);
    assert!(
        !refused.is_ok()
            && refused
                .get_psbt_error()
                .contains("derivation of change index"),
        "change without its derivation fields is refused: {}",
        refused.get_psbt_error()
    );
    assert!(
        account.active_attempt().record_id.is_empty(),
        "a refused adoption saves nothing"
    );
    assert_eq!(
        change_script(&finalized(&account, &tx)),
        change,
        "a refused adoption registers no change"
    );
    let adopted = account.adopt_transaction(tx.clone(), leaf.clone(), &first);
    assert!(adopted.is_ok(), "adopt: {}", adopted.get_psbt_error());
    let record = account.active_attempt().record_id;
    assert!(!record.is_empty(), "the adopted transaction is saved");

    let next = change_script(&finalized(&account, &tx));
    assert_ne!(next, change, "adoption registered the change index");
    let refused = account.adopt_transaction(tx.clone(), leaf, &second);
    assert!(
        !refused.is_ok()
            && refused
                .get_psbt_error()
                .contains("craft this transaction again"),
        "a second adoption of the same change index is refused: {}",
        refused.get_psbt_error()
    );
    assert_eq!(
        account.active_attempt().record_id,
        record,
        "the refusal leaves the adopted transaction active"
    );

    drop(account);
    let reopened = new_musig_account(config.account_name.clone());
    assert!(reopened.is_ok(), "reopen: {}", reopened.get_error());
    assert_eq!(
        change_script(&finalized(&reopened, &tx)),
        next,
        "the registration survives reopening"
    );

    let own_change = bitcoin::Address::from_script(&change, bitcoin::Network::Regtest)
        .expect("change address")
        .to_string();
    let to_own = template(own_change, tx.input_outpoints.clone());
    let psbt = finalized(&reopened, &to_own);
    let adopted = reopened.adopt_transaction(to_own, leaves.leaves[0].clone(), &psbt);
    assert!(
        adopted.is_ok(),
        "a recipient on a used change address is not taken for change: {}",
        adopted.get_psbt_error()
    );
    drop(reopened);
    cleanup_test_account(&config.account_name);
}

/// The leaf hashes the inputs of an unsigned PSBTv2 carry tap scripts for, sorted.
fn tap_leaves(psbt: &[u8]) -> Vec<TapLeafHash> {
    let mut leaves: Vec<_> = PsbtV2::deserialize(psbt)
        .expect("PSBTv2")
        .into_bitcoin_psbt()
        .expect("bitcoin PSBT")
        .inputs
        .iter()
        .flat_map(|input| input.tap_scripts.values())
        .map(|(script, version)| TapLeafHash::from_script(script, *version))
        .collect();
    leaves.sort();
    leaves.dedup();
    leaves
}

#[test]
fn multikey_export_restricts_to_the_spend_path_and_registers_change() {
    let (config, outpoint) = funded_with(two_leaf_descriptor(), "");
    let account = new_musig_account(config.account_name.clone());
    assert!(account.is_ok(), "open Multikey: {}", account.get_error());
    let tx = template(recipient(), vec![outpoint.clone()]);
    let leaves = account.spend_leaves(vec![outpoint]);
    assert!(leaves.is_ok, "spend leaves: {}", leaves.error);
    let leaf = leaves
        .leaves
        .iter()
        .find(|leaf| !leaf.key_path)
        .expect("a script leaf")
        .clone();
    let leaf_hash = TapLeafHash::from_str(&leaf.leaf_hash).expect("leaf hash");

    let finalized_psbt = finalized(&account, &tx);
    assert_eq!(
        tap_leaves(&finalized_psbt).len(),
        2,
        "finalize leaves every script leaf in the PSBT"
    );
    let change = change_script(&finalized_psbt);
    let exported = account.export_transaction(tx.clone(), leaf.clone(), &finalized_psbt);
    assert!(exported.is_ok(), "export: {}", exported.get_psbt_error());
    let exported = hex::decode(exported.bytes_v2_hex()).expect("exported PSBTv2 hex");
    assert_eq!(
        tap_leaves(&exported),
        vec![leaf_hash],
        "the exported PSBT is restricted to the chosen leaf"
    );

    assert_ne!(
        change_script(&finalized(&account, &tx)),
        change,
        "exporting registered the change index"
    );
    let refused = account.export_transaction(tx.clone(), leaf.clone(), &finalized_psbt);
    assert!(
        !refused.is_ok()
            && refused
                .get_psbt_error()
                .contains("already used by another transaction"),
        "a second export of the same change index is refused: {}",
        refused.get_psbt_error()
    );
    let attempt = account.active_attempt();
    assert!(
        attempt.record_id.is_empty() && attempt.psbt_v2_hex.is_empty(),
        "exporting starts no signing session"
    );
    let saved = account.saved_psbts(true);
    assert!(
        saved.is_ok && saved.entries.is_empty(),
        "exporting saves no PSBT: {} saved, {}",
        saved.entries.len(),
        saved.error
    );

    let imported = account.import_psbt(&exported);
    assert!(
        imported.is_ok && !imported.merged && !imported.record_id.is_empty(),
        "the exported PSBT imports as a new saved PSBT: {}",
        imported.error
    );
    let saved = account.saved_psbts(true);
    assert!(saved.is_ok, "list saved PSBTs: {}", saved.error);
    assert_eq!(saved.entries.len(), 1, "one saved PSBT");
    let record = &saved.entries[0].record;
    assert_eq!(
        (
            record.id.as_str(),
            record.spend.key_path,
            record.spend.leaf_hash.as_str()
        ),
        (imported.record_id.as_str(), false, leaf.leaf_hash.as_str()),
        "the saved PSBT spends with the exported leaf"
    );
    assert_eq!(
        record.current_psbt_v2_hex,
        hex::encode(&exported),
        "the saved PSBT is the exported one"
    );
    drop(account);
    cleanup_test_account(&config.account_name);
}

#[test]
fn independent_templates_craft_and_finalize_without_a_session() {
    let (config, outpoint) = funded_multikey("");
    let account = new_musig_account(config.account_name.clone());
    assert!(account.is_ok(), "open Multikey: {}", account.get_error());
    let first = template(recipient(), vec![outpoint.clone()]);
    let mut second = first.clone();
    second.outputs[0].amount = SENT / 2;

    let first_psbt = finalized(&account, &first);
    let second_psbt = finalized(&account, &second);
    let change = change_script(&first_psbt);
    let to = bitcoin::Address::from_str(&recipient())
        .expect("recipient")
        .assume_checked()
        .script_pubkey();
    let mut paid = vec![(to.clone(), SENT), (change.clone(), CHANGE)];
    paid.sort();
    assert_eq!(
        payments(&first_psbt),
        paid,
        "the first PSBT pays the first template"
    );
    let mut paid = vec![(to, SENT / 2), (change, FUNDING - SENT / 2 - FEE)];
    paid.sort();
    assert_eq!(
        payments(&second_psbt),
        paid,
        "the second PSBT pays the second template"
    );
    assert_eq!(
        payments(&finalized(&account, &first)),
        payments(&first_psbt),
        "finalizing the second template leaves the first one as it was"
    );
    assert!(
        account.craft_transaction(first).complete && account.craft_transaction(second).complete,
        "both templates craft side by side"
    );
    assert!(
        account.active_attempt().record_id.is_empty(),
        "crafting and finalizing start no signing session"
    );
    drop(account);
    cleanup_test_account(&config.account_name);
}

#[test]
fn a_restored_backup_brings_back_no_saved_psbt() {
    let (config, outpoint) = funded_multikey("");
    let name = config.account_name.clone();
    let account = new_musig_account(name.clone());
    assert!(account.is_ok(), "open Multikey: {}", account.get_error());
    let tx = template(recipient(), vec![outpoint.clone()]);
    let psbt = finalized(&account, &tx);
    let leaves = account.spend_leaves(vec![outpoint]);
    assert!(leaves.is_ok, "spend leaves: {}", leaves.error);
    let adopted = account.adopt_transaction(tx, leaves.leaves[0].clone(), &psbt);
    assert!(adopted.is_ok(), "adopt: {}", adopted.get_psbt_error());
    let saved = account.saved_psbts(true);
    assert!(
        saved.is_ok && saved.entries.len() == 1 && !account.active_attempt().record_id.is_empty(),
        "the adopted PSBT is saved and active: {}",
        saved.error
    );
    let backup = account.backup();
    assert!(backup.is_ok, "back the account up: {}", backup.error);
    drop(account);
    cleanup_test_account(&name);

    let restored = restore_account_backup(
        MUSIG_PLUGIN_ID,
        &backup.bytes,
        vec![role_keys()[0].clone()],
        String::new(),
        name.clone(),
        Network::Regtest,
    );
    assert!(restored.is_ok, "restore: {}", restored.error);
    let account = new_musig_account(name.clone());
    assert!(
        account.is_ok(),
        "open the restored account: {}",
        account.get_error()
    );
    let saved = account.saved_psbts(true);
    assert!(
        saved.is_ok && saved.entries.is_empty() && account.active_attempt().record_id.is_empty(),
        "the restored account holds no saved PSBT nor signing attempt: {}",
        saved.error
    );
    drop(account);
    cleanup_test_account(&name);
}

#[test]
fn sp_crafting_never_selects_coins() {
    let config = sp_config();
    let account = new_account(config.account_name.clone());
    assert!(account.is_ok(), "open SP: {}", account.get_error());

    let empty = template(recipient(), Vec::new());
    let crafted = account.craft_transaction(empty.clone());
    assert!(
        !crafted.complete && crafted.error.contains("No inputs"),
        "craft without inputs is incomplete: {}",
        crafted.error
    );
    let unsigned = account.finalize_transaction(empty.clone());
    assert!(
        !unsigned.is_ok && unsigned.error.contains("No inputs"),
        "finalize without inputs builds nothing: {}",
        unsigned.error
    );
    let selected = account.select_transaction_coins(empty);
    assert!(
        !selected.complete && selected.transaction.input_outpoints.is_empty(),
        "selection without coins fails and keeps the template"
    );
    let unknown = template(recipient(), vec![format!("{}:0", Txid::all_zeros())]);
    let crafted = account.craft_transaction(unknown);
    assert!(
        !crafted.complete && !crafted.error.is_empty(),
        "an unknown input is refused, not replaced"
    );

    let kinds: Vec<_> = account
        .receive_kinds()
        .into_iter()
        .map(|kind| (kind.id, kind.reusable))
        .collect();
    assert_eq!(
        kinds,
        vec![("sp".to_string(), true)],
        "a bare SP account receives on its static address"
    );
    let addresses = account.receive_addresses();
    assert_eq!(addresses.len(), 1, "the SP address only");
    assert_eq!(
        (
            addresses[0].kind.as_str(),
            addresses[0].address.clone(),
            addresses[0].descriptor.clone(),
            addresses[0].path.as_str()
        ),
        ("sp", account.sp_address(), config.get_descriptor(), "m"),
        "the SP address and what a device verifies it with"
    );
    let created = account.new_receive_address("sp");
    assert!(
        created.is_ok && created.address.address == account.sp_address(),
        "the SP kind hands out the static address: {}",
        created.error
    );
    let refused = account.new_receive_address("segwit");
    assert!(
        !refused.is_ok,
        "a kind the account lacks is refused, not substituted"
    );
    drop(account);
    cleanup_test_account(&config.account_name);
}

#[test]
fn labels_stay_visible_in_ram_after_a_failed_save() {
    for multikey in [true, false] {
        let config = if multikey {
            funded_multikey("").0
        } else {
            sp_config()
        };
        let musig = multikey.then(|| new_musig_account(config.account_name.clone()));
        let sp = (!multikey).then(|| new_account(config.account_name.clone()));
        let set = |kind: LabelKind, id: &str, text: &str| match (&musig, &sp) {
            (Some(account), _) => account.set_label(kind, id.into(), text.into()),
            (_, Some(account)) => account.set_label(kind, id.into(), text.into()),
            _ => unreachable!("one account is open"),
        };
        let labels = || {
            let snapshot = match (&musig, &sp) {
                (Some(account), _) => account.labels(),
                (_, Some(account)) => account.labels(),
                _ => unreachable!("one account is open"),
            };
            assert!(snapshot.is_ok, "label snapshot: {}", snapshot.error);
            snapshot
                .labels
                .into_iter()
                .map(|label| (label.kind.repr, label.id, label.text))
                .collect::<Vec<_>>()
        };
        let txid = Txid::from_byte_array([42; 32]).to_string();
        let outpoint = format!("{txid}:3");
        let address = recipient();
        let file = if multikey {
            "wallet/labels.json"
        } else {
            "sp/labels.json"
        };
        for (kind, id) in [
            (LabelKind::Transaction, &txid),
            (LabelKind::Outpoint, &outpoint),
            (LabelKind::Address, &address),
        ] {
            let saved = set(kind, id, "saved");
            assert!(
                saved.is_ok && saved.changed,
                "label saved, multikey={multikey}: {}",
                saved.error
            );
            common::logical::block_file(&config, file);
            let failed = set(kind, id, "in ram");
            assert!(
                !failed.is_ok && failed.changed && failed.error.contains("not saved"),
                "a failed save keeps the RAM edit, multikey={multikey}: {}",
                failed.error
            );
            assert!(
                labels().contains(&(kind.repr, id.clone(), "in ram".into())),
                "the unsaved label is visible, multikey={multikey}"
            );
            let again = set(kind, id, "in ram");
            assert!(
                !again.is_ok && !again.changed,
                "a retry with the same text changes nothing in RAM, multikey={multikey}"
            );
            common::logical::restore_file(&config, file);
            let removed = set(kind, id, "");
            assert!(
                removed.is_ok && removed.changed,
                "an empty label removes it, multikey={multikey}: {}",
                removed.error
            );
            assert!(
                !labels().iter().any(|(_, label, _)| label == id),
                "the removed label is gone, multikey={multikey}"
            );
        }
        let invalid = set(LabelKind::Outpoint, "not an outpoint", "text");
        assert!(
            !invalid.is_ok && !invalid.changed,
            "an invalid key is refused before any edit, multikey={multikey}"
        );
        drop(musig);
        drop(sp);
        cleanup_test_account(&config.account_name);
    }
}

#[derive(Clone, Copy)]
enum Answer {
    Accept,
    Reject,
    // Refused because the server already holds it.
    Known,
    Hang,
}

/// A one-connection Electrum server answering the broadcast with `answer`.
fn electrum(answer: Answer) -> (String, thread::JoinHandle<()>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("local Electrum test server");
    let url = format!("tcp://{}", listener.local_addr().expect("server address"));
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("test client connects");
        let mut reader = BufReader::new(stream.try_clone().expect("socket clone"));
        loop {
            let mut line = String::new();
            if reader.read_line(&mut line).expect("request line") == 0 {
                return;
            }
            let request: serde_json::Value = serde_json::from_str(&line).expect("JSON request");
            let reply = match (request["method"].as_str(), answer) {
                (Some("server.version"), _) => {
                    serde_json::json!({"jsonrpc":"2.0", "id":request["id"], "result":["test", "1.4"]})
                }
                (_, Answer::Accept) => {
                    serde_json::json!({"jsonrpc":"2.0", "id":request["id"], "result":"11".repeat(32)})
                }
                (_, Answer::Reject) => serde_json::json!({"jsonrpc":"2.0", "id":request["id"],
                    "error":{"code":1, "message":"bad-txns-inputs-missingorspent"}}),
                (_, Answer::Known) => serde_json::json!({"jsonrpc":"2.0", "id":request["id"],
                    "error":{"code":1, "message":
                    "sendrawtransaction RPC error: {\"code\":-27,\"message\":\"Transaction already in block chain\"}"}}),
                (_, Answer::Hang) => return,
            };
            writeln!(stream, "{reply}").expect("reply");
        }
    });
    (url, server)
}

#[test]
fn broadcast_reports_acceptance_rejection_and_uncertainty() {
    let closed = TcpListener::bind("127.0.0.1:0").expect("free port");
    let closed_url = format!("tcp://{}", closed.local_addr().expect("address"));
    drop(closed);

    let cases = [
        (Some(Answer::Accept), BroadcastOutcome::Accepted, false),
        (Some(Answer::Accept), BroadcastOutcome::Accepted, true),
        (Some(Answer::Reject), BroadcastOutcome::Rejected, false),
        (Some(Answer::Known), BroadcastOutcome::Accepted, false),
        (Some(Answer::Hang), BroadcastOutcome::Unknown, false),
        (None, BroadcastOutcome::Rejected, false),
    ];
    for (answer, outcome, block_store) in cases {
        let (url, server) = match answer {
            Some(answer) => {
                let (url, server) = electrum(answer);
                (url, Some(server))
            }
            None => (closed_url.clone(), None),
        };
        let (config, outpoint) = funded_multikey(&url);
        let account = new_musig_account_offline(&config);
        assert!(account.is_ok(), "open Multikey: {}", account.get_error());
        let spend = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::from_str(&outpoint).expect("outpoint"),
                ..Default::default()
            }],
            output: vec![TxOut {
                value: Amount::from_sat(FUNDING - FEE),
                script_pubkey: ScriptBuf::new_op_return([]),
            }],
        };
        if block_store {
            common::logical::block_file(&config, "wallet/transactions.v2.json");
        }
        let report = account.broadcast_report(&serialize(&spend));
        if block_store {
            common::logical::restore_file(&config, "wallet/transactions.v2.json");
        }
        assert_eq!(
            report.outcome, outcome,
            "broadcast outcome, blocked store {block_store}: {}",
            report.error
        );
        assert_eq!(report.txid, spend.compute_txid().to_string(), "txid");
        match outcome {
            BroadcastOutcome::Accepted => {
                assert!(report.error.is_empty(), "accepted: {}", report.error);
                assert_eq!(
                    report
                        .warnings
                        .iter()
                        .any(|w| w.contains("not saved locally")),
                    block_store,
                    "a local recording failure is a warning: {:?}",
                    report.warnings
                );
                let funds = account.funds_snapshot();
                assert!(
                    funds
                        .coins
                        .iter()
                        .any(|coin| coin.outpoint == outpoint && coin.spent),
                    "the accepted spend is recorded in RAM"
                );
            }
            BroadcastOutcome::Rejected if answer.is_none() => assert!(
                report.kind == BackendErrorKind::Network
                    && report.error.starts_with("The transaction was not sent"),
                "an unreachable server never got the transaction: {}",
                report.error
            ),
            BroadcastOutcome::Rejected => assert!(
                report.kind == BackendErrorKind::Broadcast && report.error.contains("rejected"),
                "the server refused the transaction: {}",
                report.error
            ),
            _ => assert!(
                !report.error.is_empty() && report.warnings.is_empty(),
                "a failed broadcast reports its error"
            ),
        }
        if let Some(server) = server {
            server.join().expect("test server");
        }
        drop(account);
        cleanup_test_account(&config.account_name);
    }

    let (config, _) = funded_multikey("");
    let account = new_musig_account_offline(&config);
    let invalid = account.broadcast_report(b"not a transaction");
    assert!(
        invalid.outcome == BroadcastOutcome::Rejected
            && invalid.kind == BackendErrorKind::InvalidInput
            && invalid.txid.is_empty(),
        "an undecodable transaction is never sent"
    );
    drop(account);
    cleanup_test_account(&config.account_name);
}

#[test]
fn funds_snapshot_publishes_coins_with_their_history() {
    let (config, outpoint) = funded_multikey("");
    let account = new_musig_account(config.account_name.clone());
    let funds = account.funds_snapshot();
    assert!(funds.is_ok, "funds: {}", funds.error);
    assert!(
        funds.coins.iter().any(|coin| coin.outpoint == outpoint),
        "the funded coin"
    );
    let funding = outpoint.split(':').next().expect("txid");
    assert!(
        funds.history.iter().any(|tx| tx.txid == funding) && funds.history_complete,
        "the funding transaction completes the history"
    );
    assert_eq!(
        funds.balance.unconfirmed_balance, FUNDING,
        "the balance read with them"
    );
    drop(account);
    cleanup_test_account(&config.account_name);
}

#[test]
fn a_timelocked_coin_is_not_published_spendable() {
    for (descriptor, spendable) in [(musig_descriptor(), true), (timelocked_descriptor(), false)] {
        let (config, outpoint) = funded_with(descriptor, "");
        let account = new_musig_account(config.account_name.clone());
        assert!(account.is_ok(), "open: {}", account.get_error());
        let funds = account.funds_snapshot();
        assert!(funds.is_ok, "funds: {}", funds.error);
        assert!(
            funds
                .coins
                .iter()
                .any(|coin| coin.outpoint == outpoint && !coin.spent),
            "the coin is unspent"
        );
        assert_eq!(
            funds.spendable.contains(&outpoint),
            spendable,
            "a coin with a key path is spendable, one locked behind an unmet older() is not"
        );
        drop(account);
        cleanup_test_account(&config.account_name);
    }
}

#[test]
fn endpoint_settings_test_without_saving_and_save_without_testing() {
    let (config, _) = funded_multikey("tcp://127.0.0.1:1");
    let tested = test_endpoint(
        BackendServer::Electrum,
        "not a url".into(),
        true,
        Network::Regtest,
    );
    assert!(
        !tested.is_ok && !tested.saved,
        "a bad endpoint fails its test"
    );
    let refused = save_endpoints(
        config.account_name.clone(),
        vec![EndpointSetting {
            server: BackendServer::Blindbit,
            url: "http://127.0.0.1:2".into(),
            verify_certificate: true,
        }],
    );
    assert!(
        !refused.is_ok && !refused.saved,
        "a Multikey account has no BlindBit endpoint"
    );
    assert_eq!(
        config_from_file(config.account_name.clone()).get_electrum_url(),
        "tcp://127.0.0.1:1",
        "neither call saved anything"
    );
    let saved = save_endpoints(
        config.account_name.clone(),
        vec![EndpointSetting {
            server: BackendServer::Electrum,
            url: "ssl://127.0.0.1:3".into(),
            verify_certificate: false,
        }],
    );
    assert!(
        saved.is_ok && saved.saved && !saved.verify_certificate,
        "an untested endpoint is saved: {}",
        saved.error
    );
    let reloaded = config_from_file(config.account_name.clone());
    assert_eq!(
        (
            reloaded.get_electrum_url(),
            reloaded.get_verify_certificate()
        ),
        ("ssl://127.0.0.1:3".to_string(), false),
        "the saved endpoint"
    );
    cleanup_test_account(&config.account_name);
}
