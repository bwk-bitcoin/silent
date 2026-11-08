//! Consumer uses only Silent's exported bridge API and public PSBT/Bitcoin types.

use std::{
    collections::BTreeMap,
    str::FromStr,
    thread,
    time::{Duration, Instant},
};

use bitcoin::{Address, Amount, TxOut};
use bwk::bwk_tx::bwk_psbt::{Output, PsbtV2};
use bwk_utils::test as chain;
use silent::{
    coordination_accept, coordination_prepare, coordination_release, hot_cached_nonces,
    hot_create_sessions, hot_seal, hot_sign, hot_sp_descriptor, hot_xpub, new_account, new_config,
    new_keys_module, new_musig_account, segwit_descriptor_for, state_create_with_password,
    transaction_broadcast, transaction_finalize, Network, RustChainTip, RustSelectedInput,
    RustSpendCoin, SigningProgress, SigningStage, SpendAvailability, SpendCoinStatus,
    SpendCoinsResult, SpendError, SpendLockKind, MUSIG_PLUGIN_ID,
};

const FIRST: &str =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
const SECOND: &str = "legal winner thank year wave sausage worth useful legal winner thank yellow";

fn merge<K: Ord + Clone, V: PartialEq + Clone>(
    into: &mut BTreeMap<K, V>,
    from: &BTreeMap<K, V>,
) -> Result<(), &'static str> {
    for (key, value) in from {
        if into.get(key).is_some_and(|current| current != value) {
            return Err("conflicting global field");
        }
        into.insert(key.clone(), value.clone());
    }
    Ok(())
}

fn assemble(inputs: &[RustSelectedInput], output: TxOut) -> Result<PsbtV2, &'static str> {
    let mut result: Option<PsbtV2> = None;
    for selected in inputs {
        let envelope =
            PsbtV2::deserialize(&hex::decode(&selected.variant.psbt_v2_hex).expect("envelope hex"))
                .expect("public PSBTv2 envelope");
        assert_eq!(
            envelope.inputs.len(),
            1,
            "single-input construction envelope"
        );
        assert!(
            envelope.outputs.is_empty(),
            "no exported transaction outputs"
        );
        assert_eq!(
            selected.input_index as usize,
            result.as_ref().map_or(0, |psbt| psbt.inputs.len())
        );
        let next = match &mut result {
            None => {
                result = Some(envelope);
                continue;
            }
            Some(result) => result,
        };
        merge(&mut next.xpub, &envelope.xpub)?;
        merge(&mut next.proprietary, &envelope.proprietary)?;
        merge(&mut next.unknown, &envelope.unknown)?;
        next.inputs.extend(envelope.inputs);
    }
    let mut result = result.ok_or("no inputs")?;
    result.outputs.push(Output {
        amount: output.value,
        script_pubkey: Some(output.script_pubkey),
        psbt: bitcoin::psbt::Output::default(),
    });
    let mut tx = result.unsigned_tx().map_err(|_| "unsigned transaction")?;
    tx.version = bitcoin::transaction::Version::TWO;
    tx.lock_time = bitcoin::absolute::LockTime::ZERO;
    for input in &mut tx.input {
        input.sequence = bitcoin::Sequence::MAX;
    }
    for selected in inputs {
        locks(selected)?
            .apply(&mut tx, selected.input_index as usize)
            .map_err(|_| "incompatible locks")?;
    }
    result.tx_version = tx.version;
    result.fallback_lock_time = Some(tx.lock_time);
    for (input, txin) in result.inputs.iter_mut().zip(tx.input) {
        input.sequence = txin.sequence;
    }
    Ok(result)
}

fn locks(selected: &RustSelectedInput) -> Result<bwk::bwk_descriptor::spend::Locks, &'static str> {
    let absolute = &selected.variant.absolute_lock;
    let relative = &selected.variant.relative_lock;
    let absolute = match absolute.kind {
        SpendLockKind::None => None,
        SpendLockKind::AbsoluteHeight => Some(
            bitcoin::absolute::LockTime::from_height(absolute.value)
                .map_err(|_| "absolute height")?,
        ),
        SpendLockKind::AbsoluteTime => Some(
            bitcoin::absolute::LockTime::from_time(absolute.value).map_err(|_| "absolute time")?,
        ),
        _ => return Err("absolute lock kind"),
    };
    let relative = match relative.kind {
        SpendLockKind::None => None,
        SpendLockKind::RelativeBlocks => Some(
            bitcoin::relative::LockTime::from_consensus(relative.value)
                .map_err(|_| "relative blocks")?,
        ),
        SpendLockKind::RelativeTime512 => Some(
            bitcoin::relative::LockTime::from_consensus((1 << 22) | relative.value)
                .map_err(|_| "relative time")?,
        ),
        _ => return Err("relative lock kind"),
    };
    Ok(bwk::bwk_descriptor::spend::Locks { absolute, relative })
}

fn wait_coin(mut export: impl FnMut() -> SpendCoinsResult, txid: bitcoin::Txid) -> RustSpendCoin {
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let coins = export();
        assert!(coins.is_ok, "public coin export: {}", coins.error);
        if let Some(coin) = coins
            .coins
            .into_iter()
            .find(|coin| coin.outpoint.starts_with(&txid.to_string()) && coin.has_height)
        {
            return coin;
        }
        assert!(
            Instant::now() < deadline,
            "funded coin not exported before deadline"
        );
        thread::sleep(Duration::from_millis(50));
    }
}

/// Wait until `export` reports the node's tip, and check its height, hash and
/// median-time-past against the node.
fn wait_tip(
    mut export: impl FnMut() -> SpendCoinsResult,
    node: &mut chain::corepc_node::Client,
) -> RustChainTip {
    let hash = node.best_block_hash().expect("node tip");
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let coins = export();
        assert!(coins.is_ok, "public coin export: {}", coins.error);
        if coins.has_tip && coins.tip.hash == hash.to_string() {
            let header = node
                .get_block_header_verbose(&hash)
                .expect("node tip header");
            assert_eq!(i64::from(coins.tip.height), header.height, "tip height");
            assert!(coins.tip.has_median_time_past, "regtest chain from genesis");
            assert_eq!(
                i64::from(coins.tip.median_time_past),
                header.median_time,
                "tip median-time-past"
            );
            return coins.tip;
        }
        assert!(
            Instant::now() < deadline,
            "node tip not exported before deadline"
        );
        thread::sleep(Duration::from_millis(50));
    }
}

fn encoded(psbt: &PsbtV2) -> String {
    hex::encode(psbt.serialize().expect("serialize consumer PSBT"))
}

fn corrupt_ecdsa(psbt: &str) -> String {
    let mut psbt = PsbtV2::deserialize(&hex::decode(psbt).expect("PSBT hex")).expect("PSBTv2");
    let signature = psbt.inputs[0]
        .psbt
        .partial_sigs
        .values_mut()
        .next()
        .expect("first real ECDSA signature");
    signature.signature = bitcoin::secp256k1::ecdsa::Signature::from_compact(&[1; 64])
        .expect("well-formed but incorrect signature");
    encoded(&psbt)
}

#[test]
fn conflicts_are_not_overwritten() {
    let mut map = BTreeMap::from([(1, vec![1])]);
    assert!(
        merge(&mut map, &BTreeMap::from([(1, vec![1])])).is_ok(),
        "identical metadata can be shared"
    );
    assert_eq!(
        merge(&mut map, &BTreeMap::from([(1, vec![2])])),
        Err("conflicting global field")
    );
    assert_eq!(
        map[&1],
        vec![1],
        "conflict must not replace existing metadata"
    );
}

#[test]
#[ignore = "requires local bitcoind and electrs"]
fn funded_sp_bip32_and_multikey_hot_spend_through_public_api() {
    let directory = temp_dir::TempDir::new().expect("isolated consumer data");
    silent::set_datadir(directory.path().join(".silent"));
    assert!(
        state_create_with_password("consumer password".into()).is_empty(),
        "create isolated state"
    );
    let mut backend = blindbitd::BlindbitD::new().expect("isolated regtest services");
    let endpoint = backend
        .electrsd
        .as_ref()
        .expect("Electrum service")
        .electrum_url
        .clone();
    let mut node = backend.bitcoin().expect("Bitcoin Core client");
    chain::generate_blocks(&mut node.client, 101);
    let mut keys = new_keys_module(Network::Regtest);
    let first = hot_seal(FIRST, "first", Network::Regtest);
    let second = hot_seal(SECOND, "second", Network::Regtest);
    assert!(first.is_ok() && second.is_ok(), "seal real hot keys");
    assert!(
        keys.commit_hot_key(&first, "first").is_ok,
        "register first hot route"
    );
    assert!(
        keys.commit_hot_key(&second, "second").is_ok,
        "register second hot route"
    );
    let material = keys.hot_material(&first.fingerprint());
    let sp = hot_sp_descriptor(&material, "first");
    assert!(sp.is_ok, "public SP descriptor: {}", sp.error);
    let xpub = hot_xpub(&material, "first", "m/84'/1'/0'");
    assert!(xpub.is_ok, "public BIP32 key: {}", xpub.error);
    let bip32 = segwit_descriptor_for(first.fingerprint(), xpub.xpub, Network::Regtest);
    let second_material = keys.hot_material(&second.fingerprint());
    let second_xpub = hot_xpub(&second_material, "second", "m/87'/1'/0'");
    assert!(second_xpub.is_ok, "second policy key");
    let key = format!("[{}/87'/1'/0']{}", second.fingerprint(), second_xpub.xpub);
    let policy = format!("tr({key}/<0;1>/*,pk({key}/<2;3>/*))");
    for descriptor in [&sp.descriptor, &bip32] {
        assert!(
            keys.register_descriptor(&first.fingerprint(), descriptor)
                .is_ok,
            "register first policy"
        );
    }
    assert!(
        keys.register_descriptor(&second.fingerprint(), &policy)
            .is_ok,
        "register Multikey policy"
    );
    let mut config = new_config(
        "external-sp".into(),
        Network::Regtest,
        sp.descriptor,
        backend.url(),
        endpoint.clone(),
        0,
        "sp".into(),
    );
    config.set_segwit_descriptor(bip32);
    assert!(config.save().is_empty(), "save SP/BIP32 account");
    let config = new_config(
        "external-multikey".into(),
        Network::Regtest,
        policy,
        String::new(),
        endpoint.clone(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save Multikey account");
    let mut sp_account = new_account("external-sp".into());
    let mut multikey = new_musig_account("external-multikey".into());
    assert!(sp_account.is_ok(), "SP account: {}", sp_account.get_error());
    assert!(multikey.is_ok(), "Multikey: {}", multikey.get_error());
    let first_address = Address::from_str(&sp_account.new_segwit_addr())
        .expect("SP subaccount address")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest");
    let second_address = Address::from_str(&multikey.new_addr())
        .expect("Multikey address")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest");
    let first_tx = chain::send(&mut node.client, first_address, 0.002).expect("fund BIP32");
    let second_tx =
        chain::send(&mut node.client, second_address.clone(), 0.003).expect("fund Multikey");
    chain::generate_blocks(&mut node.client, 1);
    let first_coin = wait_coin(|| sp_account.spending_coins(), first_tx);
    let second_coin = wait_coin(|| multikey.spending_coins(), second_tx);
    let tip = wait_tip(|| sp_account.spending_coins(), &mut node.client);
    let multikey_tip = wait_tip(|| multikey.spending_coins(), &mut node.client);
    assert_eq!(
        (tip.height, &tip.hash),
        (multikey_tip.height, &multikey_tip.hash),
        "both accounts read the same chain"
    );
    assert_eq!(first_coin.variants.len(), 1, "BIP32 key path");
    assert_eq!(
        second_coin.variants.len(),
        2,
        "independent Multikey key/leaf choices"
    );
    let selected = vec![
        RustSelectedInput {
            input_index: 0,
            network: first_coin.network,
            variant: first_coin.variants[0].clone(),
        },
        RustSelectedInput {
            input_index: 1,
            network: second_coin.network,
            variant: second_coin.variants[1].clone(),
        },
    ];
    assert_eq!(
        selected[1].variant.relative_lock.kind,
        SpendLockKind::None,
        "immediate leaf chosen independently"
    );
    let tx = assemble(
        &selected,
        TxOut {
            value: Amount::from_sat(first_coin.value + second_coin.value - 1_000),
            script_pubkey: second_address.script_pubkey(),
        },
    )
    .expect("compose independent envelopes");
    let unsigned_weight = tx
        .unsigned_tx()
        .expect("unsigned transaction")
        .weight()
        .to_wu();
    let prepared = coordination_prepare(
        Network::Regtest,
        &encoded(&tx),
        selected.clone(),
        SigningStage::Signatures,
    );
    assert!(
        prepared.is_ok,
        "prepare mixed input signing: {}",
        prepared.error
    );
    validate_rejections(&selected, &tx);
    let rejected = transaction_finalize(
        Network::Regtest,
        &prepared.psbt_v2_hex,
        selected.clone(),
        &prepared.session_id,
    );
    assert!(!rejected.is_ok, "unsigned transaction must not finalize");
    let unsupported = coordination_prepare(
        Network::Regtest,
        &prepared.psbt_v2_hex,
        selected.clone(),
        SigningStage::SpContributions,
    );
    assert_eq!(
        unsupported.kind,
        SpendError::Unsupported,
        "separate SP share production is explicitly unsupported"
    );
    let first_signed = hot_sign(
        &keys.hot_material(&first.fingerprint()),
        "first",
        &prepared.psbt_v2_hex,
        Vec::new(),
    );
    assert!(
        first_signed.is_ok,
        "first hot route: {}",
        first_signed.error
    );
    let partial = coordination_accept(
        &prepared.psbt_v2_hex,
        &first_signed.psbt_v2_hex,
        selected.clone(),
        SigningStage::Signatures,
    );
    let invalid_signature = coordination_accept(
        &prepared.psbt_v2_hex,
        &corrupt_ecdsa(&first_signed.psbt_v2_hex),
        selected.clone(),
        SigningStage::Signatures,
    );
    assert_eq!(
        invalid_signature.kind,
        SpendError::Integrity,
        "a missing second signer must not mask an invalid first signature as partial progress"
    );
    assert!(
        partial.is_ok,
        "accept first contribution: {}",
        partial.error
    );
    assert_eq!(
        partial.progress,
        SigningProgress::Partial,
        "one account signed, not a complete transaction"
    );
    let second_signed = hot_sign(
        &keys.hot_material(&second.fingerprint()),
        "second",
        &partial.psbt_v2_hex,
        Vec::new(),
    );
    assert!(
        second_signed.is_ok,
        "second hot route: {}",
        second_signed.error
    );
    let completed = coordination_accept(
        &partial.psbt_v2_hex,
        &second_signed.psbt_v2_hex,
        selected.clone(),
        SigningStage::Signatures,
    );
    let replaced = coordination_accept(
        &partial.psbt_v2_hex,
        &corrupt_ecdsa(&second_signed.psbt_v2_hex),
        selected.clone(),
        SigningStage::Signatures,
    );
    assert_eq!(
        replaced.kind,
        SpendError::Integrity,
        "existing signatures cannot be replaced while adding another input signature"
    );
    assert!(
        completed.is_ok,
        "accept second signature: {}",
        completed.error
    );
    assert_eq!(
        completed.progress,
        SigningProgress::Complete,
        "both independent policies satisfied"
    );
    let mut changed =
        PsbtV2::deserialize(&hex::decode(&second_signed.psbt_v2_hex).expect("signed hex"))
            .expect("signed PSBT");
    changed.outputs[0].amount = Amount::from_sat(1);
    let refused = coordination_accept(
        &partial.psbt_v2_hex,
        &encoded(&changed),
        selected.clone(),
        SigningStage::Signatures,
    );
    assert_eq!(
        refused.kind,
        SpendError::Integrity,
        "output substitution rejected"
    );
    let finalized = transaction_finalize(
        Network::Regtest,
        &completed.psbt_v2_hex,
        selected.clone(),
        &completed.session_id,
    );
    assert!(
        finalized.is_ok,
        "account-independent finalization: {}",
        finalized.error
    );
    let final_tx: bitcoin::Transaction =
        bitcoin::consensus::encode::deserialize_hex(&finalized.tx_hex).expect("final transaction");
    assert!(
        final_tx.weight().to_wu()
            <= unsigned_weight
                + 2
                + selected
                    .iter()
                    .map(|input| input.variant.satisfaction_wu)
                    .sum::<u64>(),
        "exported conservative weight bounds the real signed transaction"
    );
    let wrong_network = transaction_broadcast(Network::Bitcoin, &endpoint, &finalized.tx_hex);
    assert!(
        !wrong_network.is_ok,
        "network mismatch rejected before submission"
    );
    if let Some(binary) = std::env::var_os("SILENT_HOST_SMOKE") {
        sp_account.close_backend();
        multikey.close_backend();
        let fixture = serde_json::json!({
            "psbt": encoded(&tx), "endpoint": endpoint,
            "tip": {"height": tip.height, "hash": tip.hash, "medianTimePast": tip.median_time_past},
            "firstFingerprint": first.fingerprint(), "secondFingerprint": second.fingerprint(),
            "inputs": selected.iter().map(|selected| {
                let envelope = PsbtV2::deserialize(&hex::decode(&selected.variant.psbt_v2_hex).expect("envelope")).expect("PSBTv2");
                serde_json::json!({"outpoint": envelope.inputs[0].previous_output.to_string(), "path": selected.variant.path_id, "envelope": selected.variant.psbt_v2_hex})
            }).collect::<Vec<_>>()
        });
        let path = directory.path().join("consumer.json");
        std::fs::write(
            &path,
            serde_json::to_vec_pretty(&fixture).expect("fixture JSON"),
        )
        .expect("write public fixture");
        let output = std::process::Command::new(binary)
            .arg("--external-spend")
            .arg(&path)
            .env("HOME", directory.path())
            .env("QT_QPA_PLATFORM", "offscreen")
            .output()
            .expect("run public Qt consumer");
        assert!(
            output.status.success(),
            "public Qt consumer failed:\n{}\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        let response: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).expect("consumer result"))
                .expect("result JSON");
        assert_eq!(
            response["txid"].as_str(),
            Some(finalized.txid.as_str()),
            "public Qt workflow submits the same mixed transaction"
        );
    }
    let broadcast = transaction_broadcast(Network::Regtest, &endpoint, &finalized.tx_hex);
    assert!(
        broadcast.is_ok,
        "mixed-account network submission: {}",
        broadcast.error
    );
    assert_eq!(
        broadcast.txid,
        final_tx.compute_txid().to_string(),
        "server acknowledges exact transaction"
    );
    chain::generate_blocks(&mut node.client, 1);
    assert!(
        chain::get_tx(&mut node.client, final_tx.compute_txid()).is_some(),
        "real funded mixed spend mined"
    );
    sp_account.close_backend();
    multikey.close_backend();
    funded_musig(
        &mut keys,
        &mut node.client,
        &endpoint,
        &first.fingerprint(),
        &second.fingerprint(),
        directory.path(),
    );
    funded_sp_output(&keys, &mut node.client, &endpoint, &first.fingerprint());
}

fn validate_rejections(selected: &[RustSelectedInput], tx: &PsbtV2) {
    let mut changed = selected.to_vec();
    changed[1].network = Network::Bitcoin;
    assert_eq!(
        coordination_prepare(
            Network::Regtest,
            &encoded(tx),
            changed,
            SigningStage::Signatures
        )
        .kind,
        SpendError::InvalidRequest,
        "cross-network selection rejected"
    );
    let mut changed = selected.to_vec();
    changed[1].input_index = 0;
    assert_eq!(
        coordination_prepare(
            Network::Regtest,
            &encoded(tx),
            changed,
            SigningStage::Signatures
        )
        .kind,
        SpendError::InvalidRequest,
        "duplicate selection rejected"
    );
    let mut changed = selected.to_vec();
    changed[1].variant.path_id = selected[0].variant.path_id.clone();
    assert_eq!(
        coordination_prepare(
            Network::Regtest,
            &encoded(tx),
            changed,
            SigningStage::Signatures
        )
        .kind,
        SpendError::InvalidRequest,
        "path IDs are policy scoped"
    );
    let mut changed = tx.clone();
    changed.inputs[0]
        .psbt
        .witness_utxo
        .as_mut()
        .expect("prevout")
        .value = Amount::from_sat(1);
    assert_eq!(
        coordination_prepare(
            Network::Regtest,
            &encoded(&changed),
            selected.to_vec(),
            SigningStage::Signatures
        )
        .kind,
        SpendError::Integrity,
        "prevout substitution rejected"
    );
    let mut changed = selected.to_vec();
    for (index, input) in changed.iter_mut().enumerate() {
        let mut envelope =
            PsbtV2::deserialize(&hex::decode(&input.variant.psbt_v2_hex).expect("envelope"))
                .expect("PSBTv2");
        envelope.proprietary.insert(
            bitcoin::psbt::raw::ProprietaryKey {
                prefix: b"external-consumer".to_vec(),
                subtype: 0,
                key: Vec::new(),
            },
            vec![index as u8],
        );
        input.variant.psbt_v2_hex = encoded(&envelope);
    }
    let output = TxOut {
        value: tx.outputs[0].amount,
        script_pubkey: tx.outputs[0].script_pubkey.clone().expect("output"),
    };
    assert_eq!(
        assemble(&changed, output.clone()).expect_err("global metadata conflict"),
        "conflicting global field"
    );
    let mut locks = selected.to_vec();
    locks[0].variant.absolute_lock.kind = SpendLockKind::AbsoluteHeight;
    locks[0].variant.absolute_lock.value = 100;
    locks[1].variant.absolute_lock.kind = SpendLockKind::AbsoluteTime;
    locks[1].variant.absolute_lock.value = 500_000_100;
    assert_eq!(
        assemble(&locks, output).expect_err("height/time conflict"),
        "incompatible locks"
    );
}

fn funded_sp_output(
    keys: &silent::KeysModule,
    node: &mut chain::corepc_node::Client,
    endpoint: &str,
    fingerprint: &str,
) {
    use bwk::bwk_tx::bwk_psbt::sp::{set_sp_v0_output, PSBT_GLOBAL_SP_DLEQ};
    use bwk_sp::receiver::RecipientAddress;
    let mut account = new_account("external-sp".into());
    assert!(account.is_ok(), "reopen SP account");
    let address = Address::from_str(&account.new_segwit_addr())
        .expect("BIP32 address")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest");
    let funding = chain::send(node, address, 0.002).expect("fund SP sender");
    chain::generate_blocks(node, 1);
    let coin = wait_coin(|| account.spending_coins(), funding);
    let selected = vec![RustSelectedInput {
        input_index: 0,
        network: coin.network,
        variant: coin.variants[0].clone(),
    }];
    let mut psbt = assemble(
        &selected,
        TxOut {
            value: Amount::from_sat(coin.value - 1_000),
            script_pubkey: bitcoin::ScriptBuf::new(),
        },
    )
    .expect("SP sender envelope");
    let RecipientAddress::SpAddress(recipient) =
        RecipientAddress::parse(&account.sp_address(), bitcoin::Network::Regtest)
            .expect("public SP receiving address")
    else {
        panic!("SP address expected")
    };
    psbt.outputs[0].script_pubkey = None;
    set_sp_v0_output(
        &mut psbt.outputs[0].psbt,
        recipient.get_scan_key(),
        recipient.get_spend_key(),
        None,
    );
    let pending = coordination_prepare(
        Network::Regtest,
        &encoded(&psbt),
        selected.clone(),
        SigningStage::VerifySpContributions,
    );
    assert!(
        pending.is_ok,
        "unstarted proof stage is valid: {}",
        pending.error
    );
    assert_eq!(
        pending.progress,
        SigningProgress::Partial,
        "missing proofs are not complete"
    );
    let missing = coordination_prepare(
        Network::Regtest,
        &encoded(&psbt),
        selected.clone(),
        SigningStage::CompleteSpOutputs,
    );
    assert!(
        !missing.is_ok,
        "missing shares cannot produce output scripts"
    );
    let prepared = coordination_prepare(
        Network::Regtest,
        &encoded(&psbt),
        selected.clone(),
        SigningStage::SpCompleteAndSign,
    );
    assert!(
        prepared.is_ok,
        "supported combined SP stage: {}",
        prepared.error
    );
    let signed = hot_sign(
        &keys.hot_material(fingerprint),
        "first",
        &prepared.psbt_v2_hex,
        Vec::new(),
    );
    assert!(signed.is_ok, "combined SP signer: {}", signed.error);
    let accepted = coordination_accept(
        &prepared.psbt_v2_hex,
        &signed.psbt_v2_hex,
        selected.clone(),
        SigningStage::SpCompleteAndSign,
    );
    assert!(accepted.is_ok, "SP output integrity: {}", accepted.error);
    assert_eq!(
        accepted.progress,
        SigningProgress::Complete,
        "SP scripts/proofs and signatures complete"
    );
    let mut imported =
        PsbtV2::deserialize(&hex::decode(&accepted.psbt_v2_hex).expect("signed SP hex"))
            .expect("signed SP PSBT");
    let output_script = imported.outputs[0].script_pubkey.take();
    for input in &mut imported.inputs {
        input.psbt.partial_sigs.clear();
        input.psbt.tap_key_sig = None;
    }
    let verified = coordination_prepare(
        Network::Regtest,
        &encoded(&imported),
        selected.clone(),
        SigningStage::VerifySpContributions,
    );
    assert!(
        verified.is_ok,
        "verify real imported contribution proofs: {}",
        verified.error
    );
    assert_eq!(
        verified.progress,
        SigningProgress::StageComplete,
        "proof completion is not signing completion"
    );
    let completed = coordination_prepare(
        Network::Regtest,
        &encoded(&imported),
        selected.clone(),
        SigningStage::CompleteSpOutputs,
    );
    assert!(
        completed.is_ok,
        "independent output completion: {}",
        completed.error
    );
    let completed =
        PsbtV2::deserialize(&hex::decode(&completed.psbt_v2_hex).expect("completed hex"))
            .expect("completed PSBT");
    assert_eq!(
        completed.outputs[0].script_pubkey, output_script,
        "same proven SP destination"
    );
    let proof = imported
        .unknown
        .iter_mut()
        .find(|(key, _)| key.type_value == PSBT_GLOBAL_SP_DLEQ)
        .expect("actual global DLEQ proof")
        .1;
    proof[0] ^= 1;
    let rejected = coordination_prepare(
        Network::Regtest,
        &encoded(&imported),
        selected.clone(),
        SigningStage::VerifySpContributions,
    );
    assert!(!rejected.is_ok, "invalid DLEQ proof is rejected");
    let finalized = transaction_finalize(
        Network::Regtest,
        &accepted.psbt_v2_hex,
        selected,
        &accepted.session_id,
    );
    assert!(
        finalized.is_ok,
        "SP-aware public finalization: {}",
        finalized.error
    );
    assert!(
        transaction_broadcast(Network::Regtest, endpoint, &finalized.tx_hex).is_ok,
        "funded SP transaction submitted"
    );
    chain::generate_blocks(node, 1);
    assert!(account.start_scanner(), "start real SP receiving scan");
    let received = wait_coin(
        || account.spending_coins(),
        finalized.txid.parse().expect("SP funding txid"),
    );
    assert_eq!(
        received.account_type, "SP",
        "actual scanned Silent Payment coin"
    );
    assert!(
        received.variants[0].descriptor.is_empty(),
        "SP export contains no scan secret"
    );
    assert!(
        !received.variants[0].sp_spend_key.is_empty(),
        "public SP policy identity"
    );
    let selected = vec![RustSelectedInput {
        input_index: 0,
        network: received.network,
        variant: received.variants[0].clone(),
    }];
    let destination = Address::from_str(&account.new_segwit_addr())
        .expect("ordinary destination")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest");
    let psbt = assemble(
        &selected,
        TxOut {
            value: Amount::from_sat(received.value - 1_000),
            script_pubkey: destination.script_pubkey(),
        },
    )
    .expect("real SP input envelope");
    let prepared = coordination_prepare(
        Network::Regtest,
        &encoded(&psbt),
        selected.clone(),
        SigningStage::Signatures,
    );
    assert!(prepared.is_ok, "SP input preparation: {}", prepared.error);
    let signed = hot_sign(
        &keys.hot_material(fingerprint),
        "first",
        &prepared.psbt_v2_hex,
        Vec::new(),
    );
    assert!(signed.is_ok, "SP input hot signature: {}", signed.error);
    let accepted = coordination_accept(
        &prepared.psbt_v2_hex,
        &signed.psbt_v2_hex,
        selected.clone(),
        SigningStage::Signatures,
    );
    assert!(accepted.is_ok, "SP input response: {}", accepted.error);
    assert_eq!(accepted.progress, SigningProgress::Complete);
    let finalized = transaction_finalize(
        Network::Regtest,
        &accepted.psbt_v2_hex,
        selected,
        &accepted.session_id,
    );
    assert!(
        finalized.is_ok,
        "SP input finalization: {}",
        finalized.error
    );
    assert!(
        transaction_broadcast(Network::Regtest, endpoint, &finalized.tx_hex).is_ok,
        "real SP input spend submitted"
    );
    chain::generate_blocks(node, 1);
    let txid = finalized.txid.parse().expect("SP spend txid");
    let confirmed = wait_coin(|| account.spending_coins(), txid);
    assert_eq!(
        confirmed.variants[0].availability,
        SpendAvailability::Available,
        "known non-coinbase unlocked output"
    );
    let tip = node
        .get_best_block_hash()
        .expect("current tip")
        .0
        .parse()
        .expect("tip hash");
    node.invalidate_block(tip).expect("real chain reorg");
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let coins = account.spending_coins();
        assert!(coins.is_ok, "fresh export after reorg: {}", coins.error);
        if let Some(coin) = coins
            .coins
            .iter()
            .find(|coin| coin.outpoint == confirmed.outpoint && !coin.has_height)
        {
            assert_eq!(
                coin.status,
                SpendCoinStatus::Unconfirmed,
                "reorg removes confirmation, not the spending policy"
            );
            assert_eq!(
                coin.variants[0].path_id, confirmed.variants[0].path_id,
                "path identity survives reorg"
            );
            break;
        }
        assert!(
            Instant::now() < deadline,
            "public export retained stale confirmation after reorg"
        );
        thread::sleep(Duration::from_millis(50));
    }
    chain::generate_blocks(node, 2);
    let restored = wait_coin(|| account.spending_coins(), txid);
    // The replacement branch outgrows the invalidated tip: the export follows it.
    let reorged = wait_tip(|| account.spending_coins(), node);
    assert_ne!(
        reorged.hash,
        tip.to_string(),
        "the export reads the chain after the reorg"
    );
    assert_eq!(
        restored.variants[0].availability,
        SpendAvailability::Available,
        "availability restored from current coin state"
    );
    account.close_backend();
}

fn nonce_input_request(text: &str, index: usize) -> PsbtV2 {
    let mut request = PsbtV2::deserialize(&hex::decode(text).expect("public nonce request hex"))
        .expect("public PSBTv2");
    for (position, input) in request.inputs.iter_mut().enumerate() {
        if position != index {
            input.psbt.unknown.retain(|key, _| {
                key.type_value != bwk::bwk_tx::bwk_psbt::musig::PSBT_IN_MUSIG2_PARTICIPANT_PUBKEYS
            });
        }
    }
    request
}

fn cached_input_nonce(material: &silent::HotMaterial, text: &str, index: usize) -> String {
    // The external coordinator requests one policy's contribution, preserving the transaction.
    let request = nonce_input_request(text, index);
    let published = hot_cached_nonces(material, &encoded(&request), Vec::new());
    assert!(
        published.is_ok && !published.needs_password,
        "cached input {index} nonce: {}",
        published.error
    );
    let updated = PsbtV2::deserialize(&hex::decode(&published.psbt_v2_hex).expect("nonce hex"))
        .expect("nonce PSBT");
    let mut merged =
        PsbtV2::deserialize(&hex::decode(text).expect("original hex")).expect("original PSBT");
    merged.inputs[index] = updated.inputs[index].clone();
    encoded(&merged)
}

fn run_nonce_fixture(
    binary: std::ffi::OsString,
    directory: &std::path::Path,
    text: &str,
    selected: &[RustSelectedInput],
    fingerprint: &str,
    lifecycle: Option<bool>,
) -> serde_json::Value {
    let data = serde_json::json!({"nonceReview": lifecycle.is_none(), "lifecycleReview": lifecycle.is_some(), "retryRelease": lifecycle.unwrap_or(false), "psbt": text, "firstFingerprint": fingerprint,
        "inputs": selected.iter().map(|selected| {
            let envelope = PsbtV2::deserialize(&hex::decode(&selected.variant.psbt_v2_hex).expect("envelope")).expect("PSBTv2");
            serde_json::json!({"outpoint": envelope.inputs[0].previous_output.to_string(), "path": selected.variant.path_id, "envelope": selected.variant.psbt_v2_hex})
        }).collect::<Vec<_>>()});
    let fixture = directory.join("nonce-review.json");
    std::fs::write(
        &fixture,
        serde_json::to_vec_pretty(&data).expect("public fixture JSON"),
    )
    .expect("write public fixture");
    let output = std::process::Command::new(binary)
        .arg(if lifecycle.is_some() {
            "--external-lifecycle"
        } else {
            "--external-nonce-review"
        })
        .arg(&fixture)
        .env("HOME", directory)
        .env("QT_QPA_PLATFORM", "offscreen")
        .output()
        .expect("real IKeys nonce review");
    assert!(
        output.status.success(),
        "real IKeys nonce-stage refusal failed:\n{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    serde_json::from_slice(&std::fs::read(&fixture).expect("public nonce fixture result"))
        .expect("result JSON")
}

fn funded_musig(
    keys: &mut silent::KeysModule,
    node: &mut chain::corepc_node::Client,
    endpoint: &str,
    first: &str,
    second: &str,
    directory: &std::path::Path,
) {
    let a = hot_xpub(&keys.hot_material(first), "first", "m/87'/1'/0'");
    let b = hot_xpub(&keys.hot_material(second), "second", "m/87'/1'/0'");
    assert!(a.is_ok && b.is_ok, "MuSig public keys");
    let policy = format!(
        "tr(musig([{first}/87'/1'/0']{},[{second}/87'/1'/0']{})/<0;1>/*)",
        a.xpub, b.xpub
    );
    let other_policy = format!("tr(musig([{second}/87'/1'/0']{},[{first}/87'/1'/0']{})/<0;1>/*,pk([{first}/87'/1'/0']{}/<2;3>/*))", b.xpub, a.xpub, a.xpub);
    assert!(
        keys.register_descriptor(first, &policy).is_ok,
        "first MuSig registration"
    );
    assert!(
        keys.register_descriptor(second, &policy).is_ok,
        "second MuSig registration"
    );
    let config = new_config(
        "external-musig-rounds".into(),
        Network::Regtest,
        policy.clone(),
        String::new(),
        endpoint.into(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save MuSig account");
    let mut account = new_musig_account("external-musig-rounds".into());
    for fingerprint in [first, second] {
        assert!(
            keys.register_descriptor(fingerprint, &other_policy).is_ok,
            "register second independent MuSig policy"
        );
    }
    let config = new_config(
        "external-musig-other".into(),
        Network::Regtest,
        other_policy.clone(),
        String::new(),
        endpoint.into(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save other MuSig account");
    let mut other = new_musig_account("external-musig-other".into());
    assert!(other.is_ok(), "open other MuSig account");
    assert!(account.is_ok(), "open MuSig account");
    let address = Address::from_str(&account.new_addr())
        .expect("MuSig address")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest");
    let funding = chain::send(node, address.clone(), 0.002).expect("fund real MuSig input");
    let other_address = Address::from_str(&other.new_addr())
        .expect("other MuSig address")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest");
    let other_funding = chain::send(node, other_address, 0.002).expect("fund other policy");
    chain::generate_blocks(node, 1);
    let coin = wait_coin(|| account.spending_coins(), funding);
    let other_coin = wait_coin(|| other.spending_coins(), other_funding);
    let selected = vec![
        RustSelectedInput {
            input_index: 0,
            network: coin.network,
            variant: coin.variants[0].clone(),
        },
        RustSelectedInput {
            input_index: 1,
            network: other_coin.network,
            variant: other_coin.variants[0].clone(),
        },
    ];
    let psbt = assemble(
        &selected,
        TxOut {
            value: Amount::from_sat(coin.value + other_coin.value - 1_000),
            script_pubkey: address.script_pubkey(),
        },
    )
    .expect("MuSig envelope assembly");
    let a = keys.hot_material(first);
    let b = keys.hot_material(second);
    assert!(
        hot_create_sessions(&a, "first", &policy, 1).is_ok,
        "first nonce secret remains in its key store"
    );
    assert!(
        hot_create_sessions(&b, "second", &policy, 1).is_ok,
        "second nonce secret remains in its key store"
    );
    let prepared = coordination_prepare(
        Network::Regtest,
        &encoded(&psbt),
        selected.clone(),
        SigningStage::MusigNonces,
    );
    assert!(prepared.is_ok, "nonce round: {}", prepared.error);
    let premature = coordination_prepare(
        Network::Regtest,
        &prepared.psbt_v2_hex,
        selected.clone(),
        SigningStage::MusigSignatures,
    );
    assert!(!premature.is_ok, "round two before all nonces is rejected");
    let nonce_a = cached_input_nonce(&a, &prepared.psbt_v2_hex, 0);
    let mut invalid_nonce =
        PsbtV2::deserialize(&hex::decode(&nonce_a).expect("nonce response")).expect("PSBTv2");
    let value = invalid_nonce.inputs[0]
        .psbt
        .unknown
        .iter_mut()
        .find(|(key, _)| key.type_value == bwk::bwk_tx::bwk_psbt::musig::PSBT_IN_MUSIG2_PUB_NONCE)
        .expect("first public nonce")
        .1;
    *value = vec![0; 66];
    let rejected = coordination_accept(
        &prepared.psbt_v2_hex,
        &encoded(&invalid_nonce),
        selected.clone(),
        SigningStage::MusigNonces,
    );
    assert_eq!(
        rejected.kind,
        SpendError::Integrity,
        "correct-length non-points must not be accepted as partial nonce progress"
    );
    let partial = coordination_accept(
        &prepared.psbt_v2_hex,
        &nonce_a,
        selected.clone(),
        SigningStage::MusigNonces,
    );
    assert!(partial.is_ok, "first nonce accepted: {}", partial.error);
    assert_eq!(
        partial.progress,
        SigningProgress::Partial,
        "missing second nonce"
    );
    let nonce_b = cached_input_nonce(&b, &partial.psbt_v2_hex, 0);
    let uneven = coordination_accept(
        &partial.psbt_v2_hex,
        &nonce_b,
        selected.clone(),
        SigningStage::MusigNonces,
    );
    assert!(
        uneven.is_ok,
        "one policy's nonce round is complete: {}",
        uneven.error
    );
    assert_eq!(
        uneven.progress,
        SigningProgress::Partial,
        "other policy still needs a nonce"
    );
    let continue_nonces = coordination_prepare(
        Network::Regtest,
        &uneven.psbt_v2_hex,
        selected.clone(),
        SigningStage::MusigNonces,
    );
    assert_eq!(
        continue_nonces.kind,
        SpendError::Unsupported,
        "stage-unaware routes must refuse unequal nonce progress before side effects"
    );
    let missing_password = hot_cached_nonces(&a, &uneven.psbt_v2_hex, Vec::new());
    assert!(
        missing_password.is_ok && missing_password.needs_password,
        "real hot route needs a password with ready A and uncached B"
    );
    if let Some(binary) = std::env::var_os("SILENT_HOST_SMOKE") {
        account.close_backend();
        other.close_backend();
        run_nonce_fixture(
            binary,
            directory,
            &uneven.psbt_v2_hex,
            &selected,
            first,
            None,
        );
    }
    assert_eq!(
        cached_input_nonce(&a, &partial.psbt_v2_hex, 0),
        partial.psbt_v2_hex,
        "refused IKeys request did not retire or consume cached A nonces"
    );
    let uncached = hot_cached_nonces(
        &a,
        &encoded(&nonce_input_request(&uneven.psbt_v2_hex, 1)),
        Vec::new(),
    );
    assert!(
        uncached.is_ok && uncached.needs_password,
        "refused IKeys request did not allocate B nonces"
    );
    assert!(
        hot_create_sessions(&a, "first", &other_policy, 1).is_ok,
        "first key's other-policy nonce"
    );
    assert!(
        hot_create_sessions(&b, "second", &other_policy, 1).is_ok,
        "second key's other-policy nonce"
    );
    let other_a = cached_input_nonce(&a, &uneven.psbt_v2_hex, 1);
    let other_b = cached_input_nonce(&b, &other_a, 1);
    let nonces = coordination_accept(
        &uneven.psbt_v2_hex,
        &other_b,
        selected.clone(),
        SigningStage::MusigNonces,
    );
    assert!(nonces.is_ok, "all nonces accepted: {}", nonces.error);
    assert_eq!(
        nonces.progress,
        SigningProgress::StageComplete,
        "nonce completion is not signature completion"
    );
    let mut tampered = PsbtV2::deserialize(&hex::decode(&nonces.psbt_v2_hex).expect("nonce hex"))
        .expect("nonce PSBT");
    let nonce = tampered.inputs[0]
        .psbt
        .unknown
        .iter_mut()
        .find(|(key, _)| key.type_value == bwk::bwk_tx::bwk_psbt::musig::PSBT_IN_MUSIG2_PUB_NONCE)
        .expect("public nonce field")
        .1;
    nonce[0] ^= 1;
    let rejected = coordination_accept(
        &nonces.psbt_v2_hex,
        &encoded(&tampered),
        selected.clone(),
        SigningStage::MusigSignatures,
    );
    assert_eq!(
        rejected.kind,
        SpendError::Integrity,
        "round two cannot replace public nonces"
    );
    let round_two = coordination_prepare(
        Network::Regtest,
        &nonces.psbt_v2_hex,
        selected.clone(),
        SigningStage::MusigSignatures,
    );
    assert!(round_two.is_ok, "second round: {}", round_two.error);
    let sig_a = hot_sign(&a, "first", &round_two.psbt_v2_hex, Vec::new());
    assert!(sig_a.is_ok, "first partial: {}", sig_a.error);
    let mut invalid_partial =
        PsbtV2::deserialize(&hex::decode(&sig_a.psbt_v2_hex).expect("partial hex"))
            .expect("partial PSBT");
    let signature = invalid_partial.inputs[0]
        .psbt
        .unknown
        .iter_mut()
        .find(|(key, _)| key.type_value == bwk::bwk_tx::bwk_psbt::musig::PSBT_IN_MUSIG2_PARTIAL_SIG)
        .expect("first real partial signature")
        .1;
    *signature = vec![0; 32];
    let rejected = coordination_accept(
        &round_two.psbt_v2_hex,
        &encoded(&invalid_partial),
        selected.clone(),
        SigningStage::MusigSignatures,
    );
    assert_eq!(
        rejected.kind,
        SpendError::Integrity,
        "invalid first partial must fail before the second cosigner contributes"
    );
    let rejected = transaction_finalize(
        Network::Regtest,
        &encoded(&invalid_partial),
        selected.clone(),
        &round_two.session_id,
    );
    assert_eq!(
        rejected.kind,
        SpendError::Integrity,
        "finalization must verify present partials even when aggregation is incomplete"
    );
    let partial = coordination_accept(
        &round_two.psbt_v2_hex,
        &sig_a.psbt_v2_hex,
        selected.clone(),
        SigningStage::MusigSignatures,
    );
    assert!(partial.is_ok, "first partial accepted: {}", partial.error);
    assert_eq!(
        partial.progress,
        SigningProgress::Partial,
        "missing second partial signature"
    );
    let sig_b = hot_sign(&b, "second", &partial.psbt_v2_hex, Vec::new());
    assert!(sig_b.is_ok, "second partial: {}", sig_b.error);
    let signed = coordination_accept(
        &partial.psbt_v2_hex,
        &sig_b.psbt_v2_hex,
        selected.clone(),
        SigningStage::MusigSignatures,
    );
    assert!(signed.is_ok, "second partial accepted: {}", signed.error);
    assert_eq!(
        signed.progress,
        SigningProgress::Complete,
        "verified aggregate can finalize"
    );
    let finalized = transaction_finalize(
        Network::Regtest,
        &signed.psbt_v2_hex,
        selected.clone(),
        &signed.session_id,
    );
    assert!(finalized.is_ok, "MuSig finalization: {}", finalized.error);
    assert!(
        transaction_broadcast(Network::Regtest, endpoint, &finalized.tx_hex).is_ok,
        "funded MuSig broadcast"
    );
    chain::generate_blocks(node, 1);
    let wrong_session = coordination_release(
        Network::Regtest,
        &signed.psbt_v2_hex,
        selected.clone(),
        "not-this-attempt",
    );
    assert_eq!(
        wrong_session.kind,
        SpendError::Integrity,
        "wrong attempt identity cannot retire a session"
    );
    let released = coordination_release(
        Network::Regtest,
        &signed.psbt_v2_hex,
        selected.clone(),
        &signed.session_id,
    );
    assert!(
        released.is_ok,
        "retire completed attempt: {}",
        released.error
    );
    assert_ne!(
        released.psbt_v2_hex, prepared.psbt_v2_hex,
        "release creates a fresh attempt identity"
    );
    let stale = hot_cached_nonces(&a, &prepared.psbt_v2_hex, Vec::new());
    assert!(
        !stale.is_ok && stale.musig_session,
        "retired attempt cannot publish cached nonces again"
    );
    if let Some(binary) = std::env::var_os("SILENT_HOST_SMOKE") {
        account.close_backend();
        other.close_backend();
        for retry_release in [false, true] {
            assert!(
                hot_create_sessions(&a, "first", &policy, 1).is_ok,
                "fresh IKeys lifecycle nonce for A"
            );
            assert!(
                hot_create_sessions(&a, "first", &other_policy, 1).is_ok,
                "fresh IKeys lifecycle nonce for B"
            );
            let result = run_nonce_fixture(
                binary.clone(),
                directory,
                &encoded(&psbt),
                &selected,
                first,
                Some(retry_release),
            );
            let allocated = result["lifecyclePsbt"]
                .as_str()
                .expect("real IKeys nonce allocation result");
            let stale = hot_cached_nonces(&a, allocated, Vec::new());
            assert!(!stale.is_ok && stale.musig_session,
                    "real IKeys allocation must be retired after prep failure plus caller unload, or failed Release plus retry; retry_release={retry_release}");
        }
    }
    account.close_backend();
    other.close_backend();
}
