//! Central contracts over real isolated Regtest accounts and bwk stores.

mod common;

use std::{
    io::{BufRead, BufReader, Write},
    net::TcpListener,
    str::FromStr,
    thread,
    time::{Duration, Instant},
};

use bitcoin::{
    absolute::LockTime,
    bip32::{DerivationPath, Xpriv, Xpub},
    hashes::Hash,
    secp256k1::Secp256k1,
    transaction::Version,
    Amount, OutPoint, Transaction, TxIn, TxOut, Txid,
};
use bwk::{
    account::Account as BwkAccount, bwk_descriptor::descriptor::Descriptor,
    bwk_sign::hot_signer::HotSigner, bwk_tx::bwk_psbt::PsbtV2,
};
use silent::{
    config_from_file, new_account, new_config, new_musig_account, test_and_save_endpoint,
    BackendServer, MusigAccount, Network, NotificationFlag, Output, SavedPsbtEntry,
    TransactionTemplate, MUSIG_PLUGIN_ID,
};

use common::{descriptor_from_mnemonic, test_account_name, TEST_MNEMONIC};

fn wait_connected(mut poll: impl FnMut() -> Box<silent::Poll>) {
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline {
        let notification = poll();
        if notification.is_some()
            && notification.get_notification().flag == NotificationFlag::ElectrumConnected
        {
            return;
        }
        thread::sleep(Duration::from_millis(20));
    }
    panic!("the reopened backend did not connect to the isolated Electrum server");
}

fn descriptor() -> String {
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
    format!(
        "tr(musig({},{})/<0;1>/*,pk({}/<2;3>/*))",
        keys[0], keys[1], keys[2]
    )
}

fn entry(account: &MusigAccount) -> SavedPsbtEntry {
    let result = account.saved_psbts(true);
    assert!(result.is_ok, "saved list: {}", result.error);
    assert_eq!(result.entries.len(), 1, "exactly one saved transaction");
    result.entries.into_iter().next().expect("one entry")
}

fn encoded(psbt: &PsbtV2) -> String {
    hex::encode(psbt.serialize().expect("serialize PSBT v2"))
}

#[test]
fn builder_previews_and_active_attempt_transitions_keep_unsaved_progress() {
    let name = test_account_name();
    let config = new_config(
        name.clone(),
        Network::Regtest,
        descriptor(),
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save isolated attempt account");
    let mut bwk: BwkAccount =
        BwkAccount::try_new(config.to_musig_config().expect("backend config")).expect("backend");
    let receive = bwk.scanner_mut().new_recv_addr();
    let funding = Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::new(Txid::from_byte_array([71; 32]), 0),
            ..Default::default()
        }],
        output: vec![TxOut {
            value: Amount::from_sat(100_000),
            script_pubkey: receive.script_pubkey(),
        }],
    };
    bwk.scanner().record_unconfirmed_spend(&funding);
    drop(bwk);
    let mut account = new_musig_account(name.clone());
    assert!(
        account.is_ok(),
        "open attempt account: {}",
        account.get_error()
    );
    let outpoint = OutPoint::new(funding.compute_txid(), 0).to_string();
    let template = TransactionTemplate {
        outputs: vec![Output {
            address: account.new_addr(),
            amount: 40_000,
            label: String::new(),
            max: false,
        }],
        fee: 1000,
        fee_rate: 0.0,
        input_outpoints: vec![outpoint.clone()],
    };
    let preview = account.simulate_transaction(template.clone());
    assert!(preview.is_valid, "builder preview: {}", preview.error);
    assert_eq!(
        preview.selected_outpoints,
        vec![outpoint.clone()],
        "selected input"
    );
    assert_eq!(
        preview.selected_values,
        vec![100_000],
        "builder input value"
    );
    assert_eq!(
        preview.recipient_amounts,
        vec![40_000],
        "resolved recipients"
    );
    assert_eq!(
        (
            preview.input_total,
            preview.sending_total,
            preview.change,
            preview.fee,
            preview.transaction_total
        ),
        (100_000, 40_000, 59_000, 1000, 41_000),
        "balanced builder totals"
    );
    let mut max = template.clone();
    max.outputs[0].max = true;
    max.outputs[0].amount = 0;
    let preview = account.simulate_transaction(max);
    assert!(preview.is_valid, "MAX preview: {}", preview.error);
    assert_eq!(
        preview.recipient_amounts,
        vec![99_000],
        "builder resolved MAX"
    );
    assert_eq!(preview.change, 0, "MAX has no change");
    assert_eq!(preview.transaction_total, 100_000, "MAX plus fee");

    let leaves = account.spend_leaves(vec![outpoint]);
    assert!(leaves.is_ok, "spend leaves: {}", leaves.error);
    let prepared = account.prepare_transaction(template.clone(), leaves.leaves[0].clone());
    assert!(prepared.is_ok(), "prepare: {}", prepared.get_psbt_error());
    let snapshot = account.active_attempt();
    assert!(snapshot.is_ok, "active attempt initialized");
    assert_eq!(
        snapshot.psbt_v2_hex,
        prepared.bytes_v2_hex(),
        "active prepared bytes"
    );
    let first_id = snapshot.record_id;
    let parsed = Descriptor::from_str(&descriptor()).expect("descriptor");
    let policy = parsed.as_miniscript().expect("policy");
    let signers: Vec<_> = [1, 2, 3]
        .into_iter()
        .map(|seed| {
            HotSigner::new_from_xpriv(
                bitcoin::Network::Regtest,
                Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).expect("signer master"),
            )
        })
        .collect();
    let mut first =
        PsbtV2::deserialize(&hex::decode(prepared.bytes_v2_hex()).expect("hex")).expect("PSBT");
    let mut second = first.clone();
    signers[0].sign_v2(&mut first, policy).expect("first nonce");
    signers[1]
        .sign_v2(&mut second, policy)
        .expect("concurrent second nonce");
    let snapshot = account.accept_signed_progress(&encoded(&first));
    assert!(
        snapshot.is_ok && snapshot.tx_hex.is_empty(),
        "first nonce accepted: {}",
        snapshot.error
    );

    let path = config.account_dir().join("saved_psbts.json");
    let previous = std::fs::read(&path).expect("persisted attempt");
    let backup = path.with_extension("backup");
    std::fs::rename(&path, &backup).expect("retain persisted attempt");
    std::fs::create_dir(&path).expect("block save replacement");
    let snapshot = account.accept_signed_progress(&encoded(&second));
    assert!(
        snapshot.is_ok,
        "concurrent nonce merged despite save failure: {}",
        snapshot.error
    );
    assert!(
        !snapshot.save_error.is_empty(),
        "save failure is distinct from merge success"
    );
    assert_eq!(
        account.active_attempt().psbt_v2_hex,
        snapshot.psbt_v2_hex,
        "RAM progress retained"
    );
    assert_eq!(
        account.round_status(&snapshot.psbt_v2_hex).round,
        silent::MusigRound::PartialSigs,
        "both nonces in RAM"
    );
    std::fs::remove_dir(&path).expect("remove test save blocker");
    std::fs::rename(&backup, &path).expect("restore persisted file");
    assert_eq!(
        std::fs::read(&path).expect("old file"),
        previous,
        "failed replacement left disk unchanged"
    );
    assert!(
        account.retry_saved_psbts().save_error.is_empty(),
        "retry persists RAM progress"
    );
    let mut partial = PsbtV2::deserialize(&hex::decode(&snapshot.psbt_v2_hex).expect("nonce hex"))
        .expect("nonce PSBT");
    signers[0]
        .sign_v2(&mut partial, policy)
        .expect("first partial signature");
    let snapshot = account.accept_signed_progress(&encoded(&partial));
    assert!(
        snapshot.is_ok && snapshot.tx_hex.is_empty(),
        "one partial does not finalize: {}",
        snapshot.error
    );
    let mut partial =
        PsbtV2::deserialize(&hex::decode(&snapshot.psbt_v2_hex).expect("partial hex"))
            .expect("partial PSBT");
    signers[1]
        .sign_v2(&mut partial, policy)
        .expect("second partial signature");
    std::fs::rename(&path, &backup).expect("retain pre-finalization file");
    std::fs::create_dir(&path).expect("block finalization save");
    let finalized = account.accept_signed_progress(&encoded(&partial));
    assert!(
        finalized.is_ok && finalized.finalize_error.is_empty(),
        "complete MuSig finalizes: {} {}",
        finalized.error,
        finalized.finalize_error
    );
    assert!(
        !finalized.tx_hex.is_empty(),
        "complete MuSig produced a transaction"
    );
    assert!(
        !finalized.save_error.is_empty(),
        "finalization preserves a separate persistence failure"
    );
    assert_eq!(
        account.active_attempt().tx_hex,
        finalized.tx_hex,
        "finalized transaction is retained in RAM after save failure"
    );
    std::fs::remove_dir(&path).expect("remove finalization save blocker");
    std::fs::rename(&backup, &path).expect("restore pre-finalization file");
    assert!(
        account.retry_saved_psbts().save_error.is_empty(),
        "retry persists finalized RAM progress"
    );
    assert!(
        account.activate_saved_psbt(&first_id).is_ok,
        "activate finalized MuSig"
    );
    assert_eq!(
        account.active_attempt().tx_hex,
        finalized.tx_hex,
        "restore authoritative finalized transaction"
    );

    let mut reset_template = template.clone();
    reset_template.outputs[0].amount = 41_000;
    let prepared = account.prepare_transaction(reset_template, leaves.leaves[0].clone());
    assert!(
        prepared.is_ok(),
        "prepare reset attempt: {}",
        prepared.get_psbt_error()
    );
    let id = prepared.saved_record_id();
    let mut first = PsbtV2::deserialize(&hex::decode(prepared.bytes_v2_hex()).expect("reset hex"))
        .expect("reset PSBT");
    signers[0]
        .sign_v2(&mut first, policy)
        .expect("nonce before reset");
    assert!(
        account.accept_signed_progress(&encoded(&first)).is_ok,
        "nonce accepted before reset"
    );
    let reset = account.reset_active_attempt();
    assert!(reset.is_ok, "reset active attempt: {}", reset.error);
    assert!(
        !account.accept_signed_progress(&encoded(&first)).is_ok,
        "old attempt response refused"
    );
    assert_eq!(
        account.active_attempt().psbt_v2_hex,
        reset.psbt_v2_hex,
        "refused response leaves current attempt unchanged"
    );

    assert!(account.clear_active_attempt(), "clear runtime attempt");
    let restored = account.activate_saved_psbt(&id);
    assert!(
        restored.is_ok,
        "activate restored attempt: {}",
        restored.error
    );
    assert_eq!(
        account.active_attempt().psbt_v2_hex,
        reset.psbt_v2_hex,
        "restored authoritative bytes"
    );

    // A single-key leaf uses actual Schnorr signatures, not modal-local history.
    let prepared = account.prepare_transaction(template, leaves.leaves[1].clone());
    assert!(
        prepared.is_ok(),
        "prepare ordinary leaf: {}",
        prepared.get_psbt_error()
    );
    let mut ordinary =
        PsbtV2::deserialize(&hex::decode(prepared.bytes_v2_hex()).expect("ordinary hex"))
            .expect("ordinary PSBT");
    let plan = account.active_signing_plan(Vec::new());
    assert!(plan.is_ok, "active ordinary plan: {}", plan.error);
    assert_eq!(
        plan.candidates.len(),
        1,
        "only selected ordinary leaf signer"
    );
    let ordinary_fingerprint = plan.candidates[0].fingerprint.clone();
    assert!(
        !plan.candidates[0].signatures_done,
        "ordinary signature not yet present"
    );
    signers[2]
        .sign_v2(&mut ordinary, policy)
        .expect("ordinary signature");
    let restored_plan = silent::new_keys_module(Network::Regtest).signing_plan(&encoded(&ordinary));
    assert!(
        restored_plan.is_ok,
        "restored ordinary plan: {}",
        restored_plan.error
    );
    assert!(
        restored_plan
            .candidates
            .iter()
            .find(|candidate| candidate.fingerprint == ordinary_fingerprint)
            .expect("ordinary signer in restored plan")
            .signatures_done,
        "ordinary signature restored from PSBT"
    );
    assert!(
        account
            .update_saved_psbt(&prepared.saved_record_id(), &encoded(&ordinary))
            .is_ok,
        "save ordinary signature for restoration"
    );
    assert!(
        account
            .activate_saved_psbt(&prepared.saved_record_id())
            .is_ok,
        "restore ordinary signature"
    );
    let snapshot = account.active_attempt();
    assert!(
        snapshot.is_ok && snapshot.finalize_error.is_empty(),
        "ordinary accept/finalize: {} {}",
        snapshot.error,
        snapshot.finalize_error
    );
    assert!(
        !snapshot.tx_hex.is_empty(),
        "ordinary signature finalizes automatically"
    );
    assert!(
        account.activate_saved_psbt(&snapshot.record_id).is_ok,
        "restore finalized ordinary attempt"
    );
    assert_eq!(
        account.active_attempt().tx_hex,
        snapshot.tx_hex,
        "finalized restoration snapshot"
    );
    let receiver = account.take_receiver();
    let mut change = silent::musig_endpoint_change(account, String::new(), true, false, false);
    change.prepare_endpoint();
    assert_eq!(
        change.endpoint_phase(),
        silent::EndpointPhase::Draining,
        "Rust owns account until Qt acknowledges receiver drain"
    );
    while receiver.recv().is_some() {}
    change.finish_endpoint_after_drain();
    assert!(change.endpoint_result().is_ok, "offline reopen completes");
    let account = silent::take_musig_endpoint_account(change);
    assert_eq!(
        account.active_attempt().tx_hex,
        snapshot.tx_hex,
        "endpoint service preserves active attempt RAM"
    );
    drop(account);
    common::cleanup_test_account(&name);
}

#[cfg(unix)]
#[test]
fn label_edits_report_failed_persistence_and_retry() {
    use std::{fs, os::unix::fs::PermissionsExt, sync::Arc};

    use bwk::{
        bwk_electrum::label_store::{LabelKey, LabelStore},
        persist::{backend::json::JsonBackend, LABELS_STORE_KEY},
    };

    let _ = env_logger::builder().is_test(true).try_init();
    let txid = Txid::from_byte_array([42; 32]);
    let outpoint = OutPoint::new(txid, 0);
    for (multikey, value) in [
        (true, "retry saved"),
        (true, ""),
        (false, "retry saved"),
        (false, ""),
    ] {
        let name = test_account_name();
        let config = new_config(
            name.clone(),
            Network::Regtest,
            if multikey {
                descriptor()
            } else {
                descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest)
            },
            "http://127.0.0.1:1".into(),
            String::new(),
            0,
            if multikey { MUSIG_PLUGIN_ID } else { "sp" }.into(),
        );
        assert!(config.save().is_empty(), "save isolated label account");
        let musig = multikey.then(|| new_musig_account(name.clone()));
        let sp = (!multikey).then(|| new_account(name.clone()));
        let edit = |value: &str, saved: bool| {
            let (tx, coin) = if let Some(account) = &musig {
                assert!(account.is_ok(), "Multikey opens: {}", account.get_error());
                (
                    account.update_tx_label(txid.to_string(), value.into()),
                    account.update_coin_label(outpoint.to_string(), value.into()),
                )
            } else {
                let account = sp.as_ref().expect("SP account");
                assert!(account.is_ok(), "SP opens: {}", account.get_error());
                (
                    account.update_tx_label(txid.to_string(), value.into()),
                    account.update_coin_label(outpoint.to_string(), value.into()),
                )
            };
            for result in [tx, coin] {
                assert_eq!(
                    result.is_ok, saved,
                    "label save multikey={multikey}: {}",
                    result.error
                );
                assert_eq!(
                    result.error.is_empty(),
                    saved,
                    "storage errors cross the bridge"
                );
                if !saved {
                    assert!(
                        result.error.contains("not saved"),
                        "honest save error: {}",
                        result.error
                    );
                }
            }
        };
        edit("persisted", true);
        let directory = config.account_dir();
        let path =
            bwk::persist::cipher::sealed_path(&directory.join(format!("{LABELS_STORE_KEY}.json")));
        let original = fs::read(&path).expect("persisted encrypted labels");
        let permissions = fs::metadata(&directory)
            .expect("account directory")
            .permissions();
        fs::set_permissions(&directory, fs::Permissions::from_mode(0o500))
            .expect("deny directory writes");
        edit(value, false);
        fs::set_permissions(&directory, permissions).expect("restore directory permissions");
        assert_eq!(
            fs::read(&path).expect("labels after failure"),
            original,
            "failed edits/deletions leave disk unchanged, multikey={multikey}"
        );
        edit(value, true);
        drop(musig);
        drop(sp);
        let backend = Arc::new(JsonBackend::open(directory).expect("reopen backend"));
        let labels =
            LabelStore::load_from_backend(backend, LABELS_STORE_KEY).expect("decode saved labels");
        assert_eq!(
            labels.get(&LabelKey::Transaction(txid)).as_deref(),
            (!value.is_empty()).then_some(value),
            "retried transaction edit survives reopen, multikey={multikey}"
        );
        assert_eq!(
            labels.get(&LabelKey::OutPoint(outpoint)).as_deref(),
            (!value.is_empty()).then_some(value),
            "retried outpoint edit survives reopen, multikey={multikey}"
        );
        drop(labels);
        common::cleanup_test_account(&name);
    }
}

#[test]
fn real_store_details_labels_selected_path_signatures_and_reopen() {
    let name = test_account_name();
    let config = new_config(
        name.clone(),
        Network::Regtest,
        descriptor(),
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save isolated config");
    let mut bwk: BwkAccount = BwkAccount::try_new(config.to_musig_config().expect("bwk config"))
        .expect("real bwk account");
    let receive = bwk.scanner_mut().new_recv_addr();
    let funding = Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::new(Txid::from_byte_array([9; 32]), 1),
            ..TxIn::default()
        }],
        output: vec![TxOut {
            value: Amount::from_sat(100_000),
            script_pubkey: receive.script_pubkey(),
        }],
    };
    bwk.scanner().record_unconfirmed_spend(&funding);
    drop(bwk);
    let mut account = new_musig_account(name.clone());
    assert!(
        account.is_ok(),
        "open real persisted account: {}",
        account.get_error()
    );
    assert_eq!(account.coins().len(), 1, "real owned output loaded");
    let outpoint = OutPoint::new(funding.compute_txid(), 0).to_string();
    assert!(
        account
            .update_coin_label(outpoint.clone(), "Funding label".into())
            .is_ok,
        "outpoint mutation accepted"
    );
    assert!(
        account
            .update_tx_label(funding.compute_txid().to_string(), "Receive label".into())
            .is_ok,
        "transaction mutation accepted"
    );
    assert!(
        !account
            .update_tx_label("invalid".into(), "bad".into())
            .is_ok,
        "invalid transaction id is rejected"
    );
    assert!(
        !account
            .update_coin_label("invalid".into(), "bad".into())
            .is_ok,
        "invalid outpoint is rejected"
    );
    let details = account.tx_details(&funding.compute_txid().to_string(), false);
    assert!(
        details.is_ok,
        "stored transaction details: {}",
        details.error
    );
    assert_eq!(
        details.tx.label, "Receive label",
        "real transaction label store"
    );
    assert_eq!(
        details.outputs[0].label, "Funding label",
        "real output label store"
    );
    assert!(details.outputs[0].is_mine, "real owned receive script");
    assert!(
        !details.fee_known,
        "unknown external funding fee is not zero"
    );
    let spend = account.spend_leaves(vec![outpoint.clone()]);
    assert!(spend.is_ok, "real descriptor spend paths: {}", spend.error);
    assert_eq!(spend.leaves.len(), 2, "primary plus recovery path");
    let primary = spend.leaves[0].clone();
    assert_eq!(
        primary.fingerprints.len(),
        2,
        "only the two primary signers"
    );
    let recipient = account.new_addr();
    let prepared = account.prepare_transaction(
        TransactionTemplate {
            outputs: vec![Output {
                address: recipient,
                amount: 40_000,
                label: String::new(),
                max: false,
            }],
            fee: 1_000,
            fee_rate: 0.0,
            input_outpoints: vec![outpoint.clone()],
        },
        primary,
    );
    assert!(
        prepared.is_ok(),
        "prepare actual saved PSBT: {}",
        prepared.get_psbt_error()
    );
    let id = prepared.saved_record_id();
    assert_eq!(
        entry(&account).signatures_required,
        2,
        "not the three descriptor keys"
    );
    assert_eq!(
        entry(&account).signatures_complete,
        0,
        "new draft has no signatures"
    );
    let bytes = hex::decode(prepared.bytes_v2_hex()).expect("prepared PSBT hex");
    let mut psbt = PsbtV2::deserialize(&bytes).expect("prepared PSBT v2");
    let parsed = Descriptor::from_str(&descriptor()).expect("wallet descriptor");
    let descriptor = parsed.as_miniscript().expect("miniscript descriptor");
    let signers: Vec<_> = [1, 2]
        .into_iter()
        .map(|seed| {
            HotSigner::new_from_xpriv(
                bitcoin::Network::Regtest,
                Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).expect("signing master"),
            )
        })
        .collect();
    signers[0]
        .sign_v2(&mut psbt, descriptor)
        .expect("first signer nonce");
    signers[1]
        .sign_v2(&mut psbt, descriptor)
        .expect("second signer nonce");
    assert!(
        account.update_saved_psbt(&id, &encoded(&psbt)).is_ok,
        "persist real nonce round"
    );
    assert_eq!(
        entry(&account).signatures_complete,
        0,
        "nonces are not signatures"
    );
    signers[0]
        .sign_v2(&mut psbt, descriptor)
        .expect("first signer partial signature");
    assert!(
        account.update_saved_psbt(&id, &encoded(&psbt)).is_ok,
        "persist first partial signature"
    );
    assert_eq!(
        entry(&account).signatures_complete,
        1,
        "one signature-complete participant"
    );
    let old_receiver = account.take_receiver();
    let mut saved_config = config_from_file(name.clone());
    saved_config.set_electrum_url("ssl://127.0.0.1:1".into());
    saved_config.set_verify_certificate(false);
    assert!(
        saved_config.save().is_empty(),
        "persist complete new endpoint"
    );
    account.close_backend();
    while old_receiver.recv().is_some() {}
    let reopened = account.reopen_backend(false);
    assert!(
        reopened.is_ok,
        "reopen intentionally offline: {}",
        reopened.error
    );
    assert!(
        !account.verify_certificate(),
        "saved certificate policy is retained"
    );
    assert_eq!(
        account.electrum_url(),
        "ssl://127.0.0.1:1",
        "saved SSL scheme and endpoint retained"
    );
    assert_eq!(
        entry(&account).signatures_complete,
        1,
        "active saved signing progress survives reopen"
    );
    let offline_receiver = account.take_receiver();
    account.close_backend();
    while offline_receiver.recv().is_some() {}
    let failed_connect = account.reopen_backend(true);
    assert!(
        !failed_connect.is_ok,
        "the closed local port must not report a successful connection"
    );
    assert!(
        account.is_ok(),
        "connection failure leaves a usable offline account: {}",
        failed_connect.error
    );
    assert_eq!(
        entry(&account).signatures_complete,
        1,
        "failed connection retains signing progress"
    );
    let mut notifications = Vec::new();
    loop {
        let poll = account.try_recv();
        if !poll.is_some() {
            break;
        }
        notifications.push(poll.get_notification().flag);
    }
    assert!(
        !notifications.iter().any(|flag| matches!(
            *flag,
            NotificationFlag::ElectrumStarted | NotificationFlag::ElectrumConnected
        )),
        "offline reopen must not start a listener: {notifications:?}"
    );
    signers[1]
        .sign_v2(&mut psbt, descriptor)
        .expect("second signer partial signature");
    assert!(
        account.update_saved_psbt(&id, &encoded(&psbt)).is_ok,
        "persist second partial signature"
    );
    assert_eq!(
        entry(&account).signatures_complete,
        2,
        "both selected-path participants complete"
    );
    let finalized = account.finalize_psbt(&encoded(&psbt));
    assert!(
        finalized.is_ok,
        "finalize real MuSig signatures: {}",
        finalized.error
    );
    assert_eq!(
        entry(&account).signatures_complete,
        2,
        "finalized state retains the complete count"
    );
    let tx: Transaction =
        bitcoin::consensus::deserialize(&hex::decode(&finalized.tx_hex).expect("tx hex"))
            .expect("final transaction");
    let details = account.tx_details(&tx.compute_txid().to_string(), false);
    assert!(
        details.is_ok && details.fee_known,
        "saved PSBT supplies exact prevouts offline: {}",
        details.error
    );
    assert_eq!(details.input_total, 100_000, "actual saved input total");
    assert_eq!(details.fee, 1_000, "actual finalized fee");
    assert_eq!(
        details.inputs[0].label, "Funding label",
        "input label resolves from its real outpoint"
    );
    assert_eq!(
        details.tx.status, "finalized",
        "saved finalization is not mempool confirmation"
    );
    assert!(
        details.tx.amount_known,
        "saved funding and owned outputs give an exact amount"
    );
    assert_eq!(
        account.payment_history().len(),
        1,
        "finalized drafts are not inserted into payment history"
    );
    assert_eq!(
        details.weight,
        tx.weight().to_wu(),
        "finalized witness weight"
    );

    let recovery_address = account.new_addr();
    let recovery = account.prepare_transaction(
        TransactionTemplate {
            outputs: vec![Output {
                address: recovery_address,
                amount: 30_000,
                label: String::new(),
                max: false,
            }],
            fee: 2_000,
            fee_rate: 0.0,
            input_outpoints: vec![outpoint.clone()],
        },
        spend.leaves[1].clone(),
    );
    assert!(
        recovery.is_ok(),
        "prepare single-key recovery: {}",
        recovery.get_psbt_error()
    );
    let recovery_id = recovery.saved_record_id();
    let mut recovery_psbt =
        PsbtV2::deserialize(&hex::decode(recovery.bytes_v2_hex()).expect("recovery hex"))
            .expect("recovery PSBT");
    let recovery_signer = HotSigner::new_from_xpriv(
        bitcoin::Network::Regtest,
        Xpriv::new_master(bitcoin::Network::Regtest, &[3; 32]).expect("recovery master"),
    );
    recovery_signer
        .sign_v2(&mut recovery_psbt, descriptor)
        .expect("real recovery signature");
    assert!(
        account
            .update_saved_psbt(&recovery_id, &encoded(&recovery_psbt))
            .is_ok,
        "persist real recovery signature"
    );
    let recovery_entries = account.saved_psbts(true);
    assert!(
        recovery_entries.is_ok,
        "list recovery entries: {}",
        recovery_entries.error
    );
    let recovery_entry = recovery_entries
        .entries
        .iter()
        .find(|entry| entry.record.id == recovery_id)
        .expect("saved recovery");
    assert_eq!(
        recovery_entry.signatures_required, 1,
        "only the selected recovery signer is required"
    );
    assert_eq!(
        recovery_entry.signatures_complete, 1,
        "ordinary Schnorr signatures count without MuSig rounds"
    );
    assert!(
        account.update_coin_label(outpoint, String::new()).is_ok,
        "empty label removes the outpoint label"
    );
    assert!(
        account
            .update_tx_label(funding.compute_txid().to_string(), String::new())
            .is_ok,
        "empty label removes the history label"
    );
    drop(account);
    common::cleanup_test_account(&name);
}

#[test]
fn successful_test_saves_then_sp_reopens_offline_without_another_connection() {
    let name = test_account_name();
    let config = new_config(
        name.clone(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://127.0.0.1:1".into(),
        String::new(),
        0,
        "sp".into(),
    );
    assert!(config.save().is_empty(), "save isolated SP config");
    let mut account = new_account(name.clone());
    assert!(account.is_ok(), "real SP account: {}", account.get_error());
    let listener = TcpListener::bind("127.0.0.1:0").expect("local Electrum test server");
    let endpoint = listener.local_addr().expect("server address").to_string();
    let server_listener = listener.try_clone().expect("listener clone");
    let server = thread::spawn(move || {
        let (mut stream, _) = server_listener.accept().expect("test client connects");
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .expect("read timeout");
        let mut request = String::new();
        BufReader::new(stream.try_clone().expect("socket clone"))
            .read_line(&mut request)
            .expect("version handshake");
        let request: serde_json::Value = serde_json::from_str(&request).expect("JSON handshake");
        assert_eq!(
            request["method"], "server.version",
            "real test uses version handshake"
        );
        writeln!(stream, "{}", serde_json::json!({"jsonrpc":"2.0", "id":request["id"], "result":["test-electrum", "1.4"]})).expect("handshake response");
    });
    let receiver = account.take_receiver();
    let mut change = silent::sp_endpoint_change(
        account,
        BackendServer::Electrum,
        format!("tcp://{endpoint}"),
        false,
        true,
        false,
    );
    change.prepare_endpoint();
    let result = change.endpoint_result();
    server.join().expect("test server completed");
    assert!(
        !result.is_ok && result.saved && change.endpoint_phase() == silent::EndpointPhase::Draining,
        "passing test saves but waits for receiver drain before completion: {}",
        result.error
    );
    let saved = config_from_file(name.clone());
    assert_eq!(
        saved.get_electrum_url(),
        endpoint,
        "resolved transport saved"
    );
    assert!(
        !saved.get_verify_certificate(),
        "complete certificate policy saved"
    );
    while receiver.recv().is_some() {}
    change.finish_endpoint_after_drain();
    let reopened = change.endpoint_result();
    assert!(reopened.is_ok, "SP offline reopen: {}", reopened.error);
    assert!(
        reopened.saved && !reopened.connection_pending,
        "save outcome survives offline reopen without claiming a pending connection"
    );
    let account = silent::take_sp_endpoint_account(change);
    assert!(
        !account.electrum_running(),
        "offline SP reopen has no running Electrum connection"
    );
    listener
        .set_nonblocking(true)
        .expect("nonblocking pending connection check");
    assert!(
        matches!(listener.accept(), Err(error) if error.kind() == std::io::ErrorKind::WouldBlock),
        "save/reopen must not connect again"
    );
    let refused = test_and_save_endpoint(
        name.clone(),
        BackendServer::Electrum,
        "wss://invalid:1".into(),
        true,
    );
    assert!(
        !refused.is_ok && !refused.saved,
        "failed test does not save"
    );
    assert_eq!(
        config_from_file(name.clone()).get_electrum_url(),
        endpoint,
        "failed test preserves saved endpoint"
    );
    assert!(
        account
            .update_coin_label(
                format!("{}:0", Txid::from_byte_array([4; 32])),
                "External label".into()
            )
            .is_ok,
        "arbitrary real outpoint labels are accepted"
    );
    drop(account);
    common::cleanup_test_account(&name);
}

#[test]
#[ignore = "requires local bitcoind and electrs"]
fn both_backends_reconnect_on_the_saved_complete_endpoint() {
    let (bbd, _node, endpoint) = common::setup_blindbitd_with_electrum();
    let musig_name = test_account_name();
    let musig_config = new_config(
        musig_name.clone(),
        Network::Regtest,
        descriptor(),
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(
        musig_config.save().is_empty(),
        "save offline Multikey account"
    );
    let mut musig = new_musig_account(musig_name.clone());
    assert!(
        musig.is_ok(),
        "open Multikey account: {}",
        musig.get_error()
    );
    let saved = test_and_save_endpoint(
        musig_name.clone(),
        BackendServer::Electrum,
        endpoint.clone(),
        false,
    );
    assert!(
        saved.is_ok && saved.saved,
        "test and save Multikey endpoint: {}",
        saved.error
    );
    let old_receiver = musig.take_receiver();
    musig.close_backend();
    while old_receiver.recv().is_some() {}
    assert!(
        musig.reopen_backend(false).is_ok,
        "reopen Multikey without connecting"
    );
    let offline_receiver = musig.take_receiver();
    musig.close_backend();
    while offline_receiver.recv().is_some() {}
    let connected = musig.reopen_backend(true);
    assert!(
        connected.is_ok,
        "reconnect Multikey on saved endpoint: {}",
        connected.error
    );
    let starting_receiver = musig.take_receiver();
    musig.close_backend();
    while starting_receiver.recv().is_some() {}
    let restarted = musig.reopen_backend(true);
    assert!(
        restarted.is_ok,
        "Multikey replacement during header startup releases its lock: {}",
        restarted.error
    );
    wait_connected(|| musig.try_recv());
    assert!(
        !musig.verify_certificate(),
        "runtime reconnect retains the saved certificate choice"
    );
    drop(musig);
    common::cleanup_test_account(&musig_name);

    let sp_name = test_account_name();
    let mut sp_config = new_config(
        sp_name.clone(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        bbd.url(),
        String::new(),
        0,
        "sp".into(),
    );
    let secp = Secp256k1::new();
    let master =
        Xpriv::new_master(bitcoin::Network::Regtest, &[4; 32]).expect("SP sub-account master");
    let account_key = master
        .derive_priv(
            &secp,
            &DerivationPath::from_str("m/86'/1'/0'").expect("taproot path"),
        )
        .expect("taproot account key");
    sp_config.set_taproot_descriptor(silent::taproot_descriptor_for(
        master.fingerprint(&secp).to_string(),
        Xpub::from_priv(&secp, &account_key).to_string(),
        Network::Regtest,
    ));
    assert!(
        sp_config.save().is_empty(),
        "save SP account with real taproot sub-account"
    );
    let mut sp = new_account(sp_name.clone());
    assert!(
        sp.is_ok() && sp.has_taproot(),
        "open SP composite account: {}",
        sp.get_error()
    );
    let saved = test_and_save_endpoint(sp_name.clone(), BackendServer::Electrum, endpoint, false);
    assert!(
        saved.is_ok && saved.saved,
        "test and save SP endpoint: {}",
        saved.error
    );
    let old_receiver = sp.take_receiver();
    sp.close_backend();
    while old_receiver.recv().is_some() {}
    assert!(
        sp.reopen_backend(false).is_ok,
        "reopen SP without connecting"
    );
    let offline_receiver = sp.take_receiver();
    sp.close_backend();
    while offline_receiver.recv().is_some() {}
    let connected = sp.reopen_backend(true);
    assert!(
        connected.is_ok,
        "reconnect SP on saved endpoint: {}",
        connected.error
    );
    let starting_receiver = sp.take_receiver();
    sp.close_backend();
    while starting_receiver.recv().is_some() {}
    let restarted = sp.reopen_backend(true);
    assert!(
        restarted.is_ok,
        "SP replacement during header startup releases its lock: {}",
        restarted.error
    );
    wait_connected(|| sp.try_recv());
    assert!(
        sp.electrum_running(),
        "SP runtime reports its real live scanner connection"
    );
    drop(sp);
    common::cleanup_test_account(&sp_name);
}
