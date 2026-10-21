mod common;

use std::{
    fs,
    io::{BufRead, BufReader, Write},
    net::TcpListener,
    str::FromStr,
    thread,
    time::Duration,
};

use bitcoin::{
    absolute::LockTime,
    bip32::{ChainCode, ChildNumber, DerivationPath, Xpriv, Xpub},
    consensus::encode::deserialize_hex,
    hashes::Hash,
    secp256k1::Secp256k1,
    transaction::Version,
    Amount, NetworkKind, OutPoint, Transaction, TxIn, TxOut, Txid,
};
use bwk::{
    account::Account as BwkAccount, bwk_descriptor::descriptor::Descriptor,
    bwk_keys::nums::bip341_nums, bwk_sign::hot_signer::HotSigner, bwk_tx::bwk_psbt::PsbtV2,
};
use silent::{
    Config, KeyRouteKind, MusigAccount, MusigRound, Network, Output, TransactionTemplate,
};

#[derive(Debug, Clone, Copy)]
enum Policy {
    MultiA,
    Wpkh,
    ShWpkh,
    Wsh,
    ShWsh,
    Musig,
}

struct Funded {
    config: Box<Config>,
    account: Box<MusigAccount>,
    descriptor: Descriptor,
    signers: Vec<HotSigner>,
    fingerprints: Vec<String>,
    prepared: String,
    id: String,
}

struct VersionPeer {
    url: String,
    worker: thread::JoinHandle<()>,
}

fn version_peer() -> VersionPeer {
    let listener = TcpListener::bind("127.0.0.1:0").expect("isolated Electrum version peer");
    let url = format!("tcp://{}", listener.local_addr().expect("peer address"));
    let worker = thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("recovery test connects");
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .expect("read timeout");
        let mut request = String::new();
        BufReader::new(stream.try_clone().expect("socket clone"))
            .read_line(&mut request)
            .expect("version request");
        let request: serde_json::Value =
            serde_json::from_str(&request).expect("JSON version request");
        assert_eq!(
            request["method"], "server.version",
            "actual endpoint test handshake"
        );
        writeln!(
            stream,
            "{}",
            serde_json::json!({"jsonrpc":"2.0", "id":request["id"], "result":["review58", "1.4"]})
        )
        .expect("version response");
    });
    VersionPeer { url, worker }
}

impl Funded {
    fn new(policy: Policy) -> Self {
        let name = common::test_account_name();
        let secp = Secp256k1::new();
        let base = match policy {
            Policy::MultiA => 171,
            Policy::Wpkh => 175,
            Policy::ShWpkh => 179,
            Policy::Wsh => 183,
            Policy::ShWsh => 187,
            Policy::Musig => 195,
        };
        let count = match policy {
            Policy::Wpkh | Policy::ShWpkh => 1,
            Policy::Musig => 2,
            _ => 3,
        };
        let path = DerivationPath::from_str("m/87'/1'/0'/2").expect("role path");
        let mut keys = Vec::new();
        let mut signers = Vec::new();
        let mut fingerprints = Vec::new();
        for seed in base..base + count {
            let master = Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).expect("master");
            let role = master.derive_priv(&secp, &path).expect("role xpriv");
            let fingerprint = master.fingerprint(&secp).to_string();
            keys.push(format!(
                "[{fingerprint}/87'/1'/0'/2]{}/<0;1>/*",
                Xpub::from_priv(&secp, &role)
            ));
            fingerprints.push(fingerprint);
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
            Policy::Wpkh => format!("wpkh({})", keys[0]),
            Policy::ShWpkh => format!("sh(wpkh({}))", keys[0]),
            Policy::Wsh => format!("wsh(multi(2,{}))", keys.join(",")),
            Policy::ShWsh => format!("sh(wsh(multi(2,{})))", keys.join(",")),
            Policy::Musig => {
                let participants: Vec<_> = keys
                    .iter()
                    .map(|key| key.strip_suffix("/<0;1>/*").expect("key suffix"))
                    .collect();
                format!("tr(musig({})/<0;1>/*)", participants.join(","))
            }
        };
        let accepted = silent::validate_musig_descriptor(descriptor.clone());
        assert!(
            accepted.is_ok,
            "descriptor {policy:?} is accepted: {}",
            accepted.error
        );
        let config = silent::new_config(
            name.clone(),
            Network::Regtest,
            descriptor.clone(),
            String::new(),
            String::new(),
            0,
            "musig".into(),
        );
        assert!(config.save().is_empty(), "save {policy:?} config");
        let mut backend: BwkAccount =
            BwkAccount::try_new(config.to_musig_config().expect("backend config"))
                .expect("backend");
        let address = backend.scanner_mut().new_recv_addr();
        let funding = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::new(Txid::from_byte_array([base; 32]), 0),
                ..Default::default()
            }],
            output: vec![TxOut {
                value: Amount::from_sat(100_000),
                script_pubkey: address.script_pubkey(),
            }],
        };
        backend.scanner().record_unconfirmed_spend(&funding);
        drop(backend);
        let mut account = silent::new_musig_account(name);
        assert!(account.is_ok(), "open {policy:?}: {}", account.get_error());
        let outpoint = OutPoint::new(funding.compute_txid(), 0).to_string();
        let paths = account.spend_leaves(vec![outpoint.clone()]);
        assert!(paths.is_ok, "paths for {policy:?}: {}", paths.error);
        assert_eq!(paths.leaves.len(), 1, "one selectable path for {policy:?}");
        let recipient = account.new_addr();
        let prepared = account.prepare_transaction(
            TransactionTemplate {
                outputs: vec![Output {
                    address: recipient,
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
        assert!(
            prepared.is_ok(),
            "prepare {policy:?}: {}",
            prepared.get_psbt_error()
        );
        Self {
            config,
            account,
            descriptor: Descriptor::from_str(&descriptor).expect("descriptor"),
            signers,
            fingerprints,
            prepared: prepared.bytes_v2_hex(),
            id: prepared.saved_record_id(),
        }
    }

    fn sign(&self, signer: usize, psbt: &str) -> String {
        let mut psbt = PsbtV2::deserialize(&hex::decode(psbt).expect("PSBT hex")).expect("PSBT");
        self.signers[signer]
            .sign_v2(
                &mut psbt,
                self.descriptor.as_miniscript().expect("wallet descriptor"),
            )
            .expect("real software signature or nonce");
        hex::encode(psbt.serialize().expect("serialize signed PSBT"))
    }

    fn finish(self) {
        let name = self.config.account_name.clone();
        drop(self.account);
        common::cleanup_test_account(&name);
    }
}

fn ordinary_satisfaction(policy: Policy, restore_ready: bool) {
    let fixture = Funded::new(policy);
    let mut keys = silent::new_keys_module(Network::Regtest);
    let threshold = fixture.signers.len().min(2);
    for fingerprint in &fixture.fingerprints[..threshold] {
        if !keys.key(fingerprint).is_ok {
            assert!(
                keys.add_key(fingerprint, &format!("review58-{fingerprint}"))
                    .is_ok,
                "register available signer"
            );
            assert!(
                keys.add_route(fingerprint, KeyRouteKind::Qr).is_ok,
                "configure an available route without a physical device"
            );
        }
        assert!(
            keys.key(fingerprint).key.routes.contains(&KeyRouteKind::Qr),
            "available signer retains its configured route"
        );
    }
    let plan = fixture.account.active_signing_plan(Vec::new());
    assert!(plan.is_ok, "ordinary plan: {}", plan.error);
    for fingerprint in &fixture.fingerprints[..threshold] {
        assert!(
            plan.candidates
                .iter()
                .find(|candidate| candidate.fingerprint == *fingerprint)
                .expect("available candidate")
                .can_sign,
            "A/B are available"
        );
    }
    if fixture.fingerprints.len() == 3 {
        let third = plan
            .candidates
            .iter()
            .find(|candidate| candidate.fingerprint == fixture.fingerprints[2])
            .expect("third candidate");
        assert!(
            !third.can_sign,
            "C is unavailable and will never supply a signature"
        );
    }
    assert_eq!(
        fixture.account.round_status(&fixture.prepared).round,
        MusigRound::NotMusig,
        "ordinary path has no MuSig round"
    );
    let first = fixture
        .account
        .accept_signed_progress(&fixture.sign(0, &fixture.prepared));
    assert!(
        first.is_ok && first.finalize_error.is_empty(),
        "accept A: {} {}",
        first.error,
        first.finalize_error
    );
    let complete = if threshold == 2 {
        assert!(
            first.tx_hex.is_empty(),
            "one signature does not satisfy 2-of-3"
        );
        let answer = fixture.sign(1, &first.psbt_v2_hex);
        if restore_ready {
            assert!(
                fixture
                    .account
                    .update_saved_psbt(&fixture.id, &answer)
                    .is_ok,
                "persist ready A/B PSBT before restoring it"
            );
            assert!(
                fixture.account.clear_active_attempt(),
                "close runtime attempt"
            );
            let restored = fixture.account.activate_saved_psbt(&fixture.id);
            assert!(
                restored.is_ok,
                "activate ready threshold PSBT: {}",
                restored.error
            );
            fixture.account.active_attempt()
        } else {
            fixture.account.accept_signed_progress(&answer)
        }
    } else {
        first
    };
    assert!(
        complete.is_ok && complete.finalize_error.is_empty(),
        "ordinary finalization: {} {}",
        complete.error,
        complete.finalize_error
    );
    assert!(
        !complete.tx_hex.is_empty(),
        "satisfiable {policy:?} finalizes without C"
    );
    let tx: Transaction = deserialize_hex(&complete.tx_hex).expect("final transaction");
    assert!(
        tx.input
            .iter()
            .all(|input| !input.witness.is_empty() || !input.script_sig.is_empty()),
        "real finalized satisfaction"
    );
    assert_eq!(
        tx.output
            .iter()
            .map(|output| output.value.to_sat())
            .sum::<u64>(),
        99_000,
        "actual outputs and fee"
    );
    let restored = fixture.account.activate_saved_psbt(&fixture.id);
    assert!(
        restored.is_ok,
        "restore finalized ordinary transaction: {}",
        restored.error
    );
    assert_eq!(
        fixture.account.active_attempt().tx_hex,
        complete.tx_hex,
        "restored finalized snapshot"
    );
    fixture.finish();
}

#[test]
fn multi_a_two_of_three_finalizes_with_only_available_a_and_b() {
    ordinary_satisfaction(Policy::MultiA, false);
    ordinary_satisfaction(Policy::MultiA, true);
}

#[test]
fn accepted_non_taproot_descriptors_use_actual_satisfaction() {
    for policy in [Policy::Wpkh, Policy::ShWpkh, Policy::Wsh, Policy::ShWsh] {
        ordinary_satisfaction(policy, false);
    }
}

#[test]
fn archive_write_failure_does_not_clear_required_restart_fence() {
    let fixture = Funded::new(Policy::Musig);
    let first_nonce = fixture.sign(0, &fixture.prepared);
    let second_nonce = fixture.sign(1, &fixture.prepared);
    let progress = fixture.account.accept_signed_progress(&first_nonce);
    assert!(progress.is_ok, "initial nonce progress: {}", progress.error);
    assert!(
        fixture.account.require_active_restart().reset_pending,
        "Rust requires restart after session loss"
    );
    let path = fixture.config.account_dir().join("saved_psbts.json");
    let backup = path.with_extension("review58-backup");
    fs::rename(&path, &backup).expect("retain persisted attempt");
    fs::create_dir(&path).expect("block durable archive replacement");
    let failed = fixture.account.reset_active_attempt();
    assert!(
        !failed.is_ok && failed.reset_pending,
        "failed reset preserves required restart: {}",
        failed.error
    );
    let stored = fixture.account.restore_saved_psbt(&fixture.id);
    assert!(
        stored.is_ok && !stored.entry.record.reset_pending,
        "failed archive rolled the store fence back"
    );
    assert!(
        fixture.account.active_attempt().reset_pending,
        "Rust active fence survives store rollback"
    );
    let plan = fixture.account.active_signing_plan(Vec::new());
    assert!(
        !plan.is_ok,
        "Rust refuses an active signing plan while restart is required"
    );
    assert_eq!(
        plan.error, "Active attempt requires reset",
        "the refusal is the restart fence, not device availability"
    );
    let refused = fixture.account.accept_signed_progress(&second_nonce);
    assert!(
        !refused.is_ok,
        "Rust refuses a valid old nonce response after failed reset"
    );
    assert_eq!(
        refused.error, "Active attempt cannot accept signing progress",
        "acceptance is fenced before merge"
    );
    assert_eq!(
        fixture.account.active_attempt().psbt_v2_hex,
        progress.psbt_v2_hex,
        "refused progress cannot mutate active bytes"
    );
    fs::remove_dir(&path).expect("remove archive blocker");
    fs::rename(&backup, &path).expect("restore persisted attempt");
    let reset = fixture.account.reset_active_attempt();
    assert!(
        reset.is_ok && !reset.reset_pending,
        "successful fresh reset clears required restart: {}",
        reset.error
    );
    assert_ne!(
        reset.psbt_v2_hex, progress.psbt_v2_hex,
        "fresh attempt bytes"
    );
    assert!(
        !fixture.account.active_attempt().reset_pending,
        "authoritative active fence is cleared only on success"
    );
    let stored = fixture.account.restore_saved_psbt(&fixture.id);
    assert!(stored.is_ok, "fresh attempt is readable: {}", stored.error);
    assert_eq!(
        stored.entry.record.attempt_id, 1,
        "successful reset installs the next attempt identity"
    );
    assert_eq!(
        stored.entry.record.archived_attempts.len(),
        1,
        "failed archive did not leave a phantom archived attempt"
    );
    assert!(
        fixture.account.active_signing_plan(Vec::new()).is_ok,
        "fresh attempt can be planned"
    );
    assert!(
        !fixture.account.accept_signed_progress(&second_nonce).is_ok,
        "old response remains fenced by fresh attempt identity"
    );
    fixture.finish();
}

#[test]
fn fresh_endpoint_command_recovers_closed_musig_account_and_active_attempt() {
    let mut fixture = Funded::new(Policy::Musig);
    let before = fixture.account.active_attempt();
    let receiver = fixture.account.take_receiver();
    let mut failed =
        silent::musig_endpoint_change(fixture.account, String::new(), true, false, false);
    failed.prepare_endpoint();
    assert_eq!(
        failed.endpoint_phase(),
        silent::EndpointPhase::Draining,
        "initial command closes the live account"
    );
    while receiver.recv().is_some() {}
    let path = fixture.config.config_path();
    let backup = path.with_extension("review58-config-backup");
    fs::rename(&path, &backup).expect("retain isolated MuSig config");
    failed.finish_endpoint_after_drain();
    assert!(
        !failed.endpoint_result().is_ok,
        "unreadable config prevents reopen"
    );
    assert_eq!(
        failed.endpoint_result().kind,
        silent::BackendErrorKind::Storage,
        "source config error"
    );
    let closed = silent::take_musig_endpoint_account(failed);
    assert!(!closed.is_ok(), "failed command returns a closed account");
    assert_eq!(
        closed.active_attempt().psbt_v2_hex,
        before.psbt_v2_hex,
        "closed account retains active RAM progress"
    );
    fs::rename(&backup, &path).expect("restore isolated MuSig config");
    let peer = version_peer();
    let mut retry = silent::musig_endpoint_change(closed, peer.url, true, true, false);
    retry.prepare_endpoint();
    assert_eq!(
        retry.endpoint_phase(),
        silent::EndpointPhase::Draining,
        "fresh command uses the saved name rather than rejecting is_ok=false"
    );
    peer.worker.join().expect("recovery handshake completed");
    retry.finish_endpoint_after_drain();
    assert!(
        retry.endpoint_result().is_ok,
        "public MuSig command recovers: {}",
        retry.endpoint_result().error
    );
    assert!(
        retry.endpoint_result().saved,
        "test/save recovery uses the retained human account name"
    );
    fixture.account = silent::take_musig_endpoint_account(retry);
    assert!(
        fixture.account.is_ok() && fixture.account.get_error().is_empty(),
        "recovered account is ready without a stale error"
    );
    assert_eq!(
        fixture.account.name(),
        fixture.config.account_name,
        "MuSig recovery identity"
    );
    assert_eq!(
        fixture.account.active_attempt().psbt_v2_hex,
        before.psbt_v2_hex,
        "command recovery preserves active RAM bytes"
    );
    assert!(
        fixture.account.restore_saved_psbt(&fixture.id).is_ok,
        "closed saved store is recovered with the account"
    );
    fixture.finish();
}

#[test]
fn endpoint_commands_still_refuse_accounts_that_never_initialized() {
    let name = common::test_account_name();
    let sp = silent::new_account(name.clone());
    assert!(!sp.is_ok(), "missing SP account did not initialize");
    let mut sp = silent::sp_endpoint_change(
        sp,
        silent::BackendServer::Electrum,
        String::new(),
        true,
        false,
        false,
    );
    sp.prepare_endpoint();
    assert_eq!(
        sp.endpoint_phase(),
        silent::EndpointPhase::Complete,
        "uninitialized SP has no recovery command"
    );
    assert!(
        !sp.endpoint_result().is_ok && !sp.endpoint_result().error.is_empty(),
        "SP refusal is explicit"
    );
    let musig = silent::new_musig_account(name);
    assert!(!musig.is_ok(), "missing MuSig account did not initialize");
    let mut musig = silent::musig_endpoint_change(musig, String::new(), true, false, false);
    musig.prepare_endpoint();
    assert_eq!(
        musig.endpoint_phase(),
        silent::EndpointPhase::Complete,
        "uninitialized MuSig has no recovery command"
    );
    assert!(
        !musig.endpoint_result().is_ok && !musig.endpoint_result().error.is_empty(),
        "MuSig refusal is explicit"
    );
}
