//! Account-independent validation, signing coordination and network submission.

use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    str::FromStr,
    time::Duration,
};

use bitcoin::{
    consensus::encode::{deserialize_hex, serialize_hex},
    hashes::{hash160, Hash},
    secp256k1::Secp256k1,
    sighash::{Prevouts, SighashCache},
};
use bwk::{
    bwk_descriptor::{
        spend::{restrict_variant, spend_variants},
        wallet::WalletDescriptor,
    },
    bwk_sign::{
        bwk_musig::{nonce_agg, partial_verify, rust_bitcoin::RustBitcoin},
        miniscript::{
            psbt::{Error as PsbtError, InputError, PsbtExt},
            Error as MiniscriptError,
        },
        musig::{
            aggregate_partial_sigs, musig_status, slot_infos, slot_sighash, MusigRound, SlotInfo,
        },
    },
    bwk_tx::bwk_psbt::{
        musig::{
            musig_input_partial_sig, musig_input_partial_sigs, musig_input_pub_nonce,
            musig_input_pub_nonces, remove_musig_input_partial_sigs, remove_musig_input_pub_nonces,
            PSBT_IN_MUSIG2_PARTIAL_SIG, PSBT_IN_MUSIG2_PUB_NONCE,
        },
        sp::{
            PSBT_GLOBAL_SP_DLEQ, PSBT_GLOBAL_SP_ECDH_SHARE, PSBT_IN_SP_DLEQ, PSBT_IN_SP_ECDH_SHARE,
        },
        PsbtV2, TxModifiable,
    },
};
use bwk_sp::account::bip375;

use crate::{
    ffi::{
        CoordinationResult, Network, RustSelectedInput, SigningProgress, SigningStage, SpendError,
        SpendLockKind,
    },
    keys::{
        bridge::{merge_signed_psbt, musig_session_restart},
        musig_sessions,
    },
    spending::{preimage_requirement, SP_SATISFACTION_WU},
};

type Failure = (SpendError, String);

fn invalid(message: impl ToString) -> Failure {
    (SpendError::InvalidRequest, message.to_string())
}

fn result(value: Result<CoordinationResult, Failure>) -> CoordinationResult {
    value.unwrap_or_else(|(kind, error)| CoordinationResult {
        is_ok: false,
        error,
        kind,
        psbt_v2_hex: String::new(),
        progress: SigningProgress::Partial,
        tx_hex: String::new(),
        txid: String::new(),
        session_id: String::new(),
    })
}

fn success(psbt: &PsbtV2, progress: SigningProgress) -> Result<CoordinationResult, Failure> {
    Ok(CoordinationResult {
        is_ok: true,
        error: String::new(),
        kind: SpendError::Other,
        psbt_v2_hex: hex::encode(psbt.serialize().map_err(invalid)?),
        progress,
        tx_hex: String::new(),
        txid: String::new(),
        session_id: musig_sessions::attempt_id(&psbt.proprietary)
            .map_err(invalid)?
            .map(hex::encode)
            .unwrap_or_default(),
    })
}

fn decode(text: &str) -> Result<PsbtV2, Failure> {
    PsbtV2::deserialize(&hex::decode(text).map_err(invalid)?).map_err(invalid)
}

fn unsigned_input(input: &bitcoin::psbt::Input) -> bitcoin::psbt::Input {
    let mut input = input.clone();
    input.partial_sigs.clear();
    input.tap_key_sig = None;
    input.tap_script_sigs.clear();
    input.final_script_sig = None;
    input.final_script_witness = None;
    input.ripemd160_preimages.clear();
    input.sha256_preimages.clear();
    input.hash160_preimages.clear();
    input.hash256_preimages.clear();
    remove_musig_input_pub_nonces(&mut input);
    remove_musig_input_partial_sigs(&mut input);
    input
        .unknown
        .retain(|key, _| !matches!(key.type_value, PSBT_IN_SP_ECDH_SHARE | PSBT_IN_SP_DLEQ));
    input
}

fn policies(inputs: &[RustSelectedInput]) -> Result<Vec<WalletDescriptor>, Failure> {
    inputs
        .iter()
        .filter(|input| !input.variant.descriptor.is_empty())
        .map(|input| input.variant.descriptor.clone())
        .collect::<BTreeSet<_>>()
        .into_iter()
        .map(|text| WalletDescriptor::from_str(&text).map_err(invalid))
        .collect()
}

fn validate_inputs(
    network: Network,
    psbt: &PsbtV2,
    inputs: &[RustSelectedInput],
) -> Result<(), Failure> {
    if inputs.len() != psbt.inputs.len() || inputs.is_empty() {
        return Err(invalid(
            "select exactly one exported variant for every input",
        ));
    }
    let mut seen = BTreeSet::new();
    let mut outpoints = BTreeSet::new();
    for selected in inputs {
        if selected.network != network || !seen.insert(selected.input_index) {
            return Err(invalid("mixed networks or duplicate input selection"));
        }
        let input = psbt
            .inputs
            .get(selected.input_index as usize)
            .ok_or_else(|| invalid("input index out of bounds"))?;
        if !outpoints.insert(input.previous_output) {
            return Err(invalid("duplicate input outpoint"));
        }
        let template = decode(&selected.variant.psbt_v2_hex)?;
        if template.inputs.len() != 1
            || !template.outputs.is_empty()
            || selected.variant.path_id.is_empty()
        {
            return Err(invalid("invalid construction envelope or empty path ID"));
        }
        let expected = &template.inputs[0];
        if template.unknown.keys().any(|key| {
            matches!(
                key.type_value,
                PSBT_GLOBAL_SP_DLEQ | PSBT_GLOBAL_SP_ECDH_SHARE
            )
        }) || unsigned_input(&expected.psbt) != expected.psbt
            || musig_sessions::attempt_id(&template.proprietary)
                .map_err(invalid)?
                .is_some()
        {
            return Err(invalid(
                "construction envelope contains signing progress or an attempt identity",
            ));
        }
        validate_path(&template, selected)?;
        validate_preimages(&input.psbt, &selected.variant.required_preimages)?;
        validate_input_metadata(input, expected)?;
        for (key, value) in &template.xpub {
            if psbt.xpub.get(key) != Some(value) {
                return Err(invalid("missing or conflicting global xpub"));
            }
        }
        for (key, value) in &template.proprietary {
            if psbt.proprietary.get(key) != Some(value) {
                return Err(invalid("missing or conflicting global proprietary field"));
            }
        }
        for (key, value) in &template.unknown {
            if psbt.unknown.get(key) != Some(value) {
                return Err(invalid("missing or conflicting global field"));
            }
        }
        validate_locks(psbt, selected)?;
    }
    Ok(())
}

fn validate_path(template: &PsbtV2, selected: &RustSelectedInput) -> Result<(), Failure> {
    let path = &selected.variant;
    if path.descriptor.is_empty() {
        return validate_sp_path(template, selected);
    }
    let descriptor = WalletDescriptor::from_str(&path.descriptor).map_err(invalid)?;
    if !path.sp_spend_key.is_empty() {
        return Err(invalid(
            "input cannot name both a descriptor and an SP policy",
        ));
    }
    let variants = spend_variants(&descriptor, path.keychain as usize, path.derivation_index)
        .map_err(invalid)?;
    let variant = variants
        .variants
        .iter()
        .find(|variant| variant.id.to_string() == path.path_id)
        .ok_or_else(|| invalid("path ID does not belong to this policy"))?;
    let absolute: crate::ffi::RustSpendLock = variant.requirements.locks.absolute.into();
    let relative: crate::ffi::RustSpendLock = variant.requirements.locks.relative.into();
    if absolute.kind != path.absolute_lock.kind
        || absolute.value != path.absolute_lock.value
        || relative.kind != path.relative_lock.kind
        || relative.value != path.relative_lock.value
        || variant.satisfaction_wu != path.satisfaction_wu
        || variant
            .requirements
            .keys
            .iter()
            .map(ToString::to_string)
            .collect::<Vec<_>>()
            != path.required_keys
        || variant
            .requirements
            .preimages
            .iter()
            .map(preimage_requirement)
            .collect::<Vec<_>>()
            != path.required_preimages
    {
        return Err(invalid("path requirements differ from the descriptor"));
    }
    let original = template.clone().into_bitcoin_psbt().map_err(invalid)?;
    let mut restricted = original.clone();
    restrict_variant(
        &descriptor,
        path.keychain as usize,
        path.derivation_index,
        &variant.id,
        &mut restricted,
        0,
    )
    .map_err(invalid)?;
    if original != restricted {
        return Err(invalid(
            "construction envelope is not restricted to its selected path",
        ));
    }
    if variants
        .variants
        .iter()
        .filter(|other| other.id.path == variant.id.path)
        .count()
        > 1
    {
        return Err((
            SpendError::Unsupported,
            "current signing routes cannot enforce an alternative satisfaction within one script"
                .into(),
        ));
    }
    Ok(())
}

fn validate_input_metadata(
    input: &bwk::bwk_tx::bwk_psbt::Input,
    expected: &bwk::bwk_tx::bwk_psbt::Input,
) -> Result<(), Failure> {
    let mut metadata = unsigned_input(&input.psbt);
    if expected.psbt.sighash_type.is_none() {
        metadata.sighash_type = None;
    }
    if expected.psbt.non_witness_utxo.is_none() {
        if let Some(transaction) = &metadata.non_witness_utxo {
            if transaction.compute_txid() != input.previous_output.txid
                || transaction.output.get(input.previous_output.vout as usize)
                    != expected.psbt.witness_utxo.as_ref()
            {
                return Err(signature_error(
                    "full previous transaction does not match the exported prevout",
                ));
            }
        }
        metadata.non_witness_utxo = None;
    }
    if input.previous_output != expected.previous_output || metadata != expected.psbt {
        return Err(signature_error(
            "selected input metadata does not match its construction envelope",
        ));
    }
    Ok(())
}

fn validate_preimages(input: &bitcoin::psbt::Input, required: &[String]) -> Result<(), Failure> {
    check_preimages(&input.ripemd160_preimages, "ripemd160", required)?;
    check_preimages(&input.sha256_preimages, "sha256", required)?;
    check_preimages(&input.hash160_preimages, "hash160", required)?;
    check_preimages(&input.hash256_preimages, "hash256", required)
}

fn check_preimages<H: Hash + Ord>(
    preimages: &BTreeMap<H, Vec<u8>>,
    kind: &str,
    required: &[String],
) -> Result<(), Failure> {
    for (hash, preimage) in preimages {
        let requirement = format!("{kind}:{}", hex::encode(&hash[..]));
        if preimage.len() != 32
            || <H as Hash>::hash(preimage) != *hash
            || !required.contains(&requirement)
        {
            return Err(invalid(
                "preimage does not satisfy a selected path requirement",
            ));
        }
    }
    Ok(())
}

fn validate_sp_path(template: &PsbtV2, selected: &RustSelectedInput) -> Result<(), Failure> {
    use bwk::bwk_tx::bwk_psbt::sp::{sp_input_spend_bip32_derivation, sp_input_tweak};
    let path = &selected.variant;
    let public_key =
        bitcoin::secp256k1::PublicKey::from_str(&path.sp_spend_key).map_err(invalid)?;
    if path.path_id != format!("sp-spend({public_key}):Key:0")
        || path.satisfaction_wu != SP_SATISFACTION_WU
        || path.absolute_lock.kind != SpendLockKind::None
        || path.relative_lock.kind != SpendLockKind::None
    {
        return Err(invalid("invalid SP spending requirements"));
    }
    let input = &template.inputs[0].psbt;
    let key = match sp_input_spend_bip32_derivation(input, public_key).map_err(invalid)? {
        Some((fingerprint, derivation)) => format!("[{fingerprint}/{derivation}]{public_key}"),
        None => public_key.to_string(),
    };
    if path.required_keys != vec![key] || !path.required_preimages.is_empty() {
        return Err(invalid(
            "SP key requirements do not match the construction envelope",
        ));
    }
    let tweak = sp_input_tweak(input)
        .map_err(invalid)?
        .ok_or_else(|| invalid("missing SP input tweak"))?;
    let key = public_key
        .add_exp_tweak(
            &Secp256k1::verification_only(),
            &bitcoin::secp256k1::Scalar::from_be_bytes(tweak).map_err(invalid)?,
        )
        .map_err(invalid)?
        .x_only_public_key()
        .0;
    let script = bitcoin::ScriptBuf::new_p2tr_tweaked(
        bitcoin::key::TweakedPublicKey::dangerous_assume_tweaked(key),
    );
    if input.witness_utxo.as_ref().map(|utxo| &utxo.script_pubkey) != Some(&script) {
        return Err(invalid(
            "SP spend key/tweak does not match the input prevout",
        ));
    }
    Ok(())
}

fn validate_locks(psbt: &PsbtV2, selected: &RustSelectedInput) -> Result<(), Failure> {
    use bwk::bwk_descriptor::spend::Locks;
    let absolute = match selected.variant.absolute_lock.kind {
        SpendLockKind::None => None,
        SpendLockKind::AbsoluteHeight => Some(
            bitcoin::absolute::LockTime::from_height(selected.variant.absolute_lock.value)
                .map_err(invalid)?,
        ),
        SpendLockKind::AbsoluteTime => Some(
            bitcoin::absolute::LockTime::from_time(selected.variant.absolute_lock.value)
                .map_err(invalid)?,
        ),
        _ => return Err(invalid("invalid absolute lock kind")),
    };
    let value = selected.variant.relative_lock.value;
    let relative = match selected.variant.relative_lock.kind {
        SpendLockKind::None => None,
        SpendLockKind::RelativeBlocks => Some(
            bitcoin::relative::LockTime::from_consensus(u32::from(
                u16::try_from(value).map_err(invalid)?,
            ))
            .map_err(invalid)?,
        ),
        SpendLockKind::RelativeTime512 => Some(
            bitcoin::relative::LockTime::from_consensus(
                (1 << 22) | u32::from(u16::try_from(value).map_err(invalid)?),
            )
            .map_err(invalid)?,
        ),
        _ => return Err(invalid("invalid relative lock kind")),
    };
    // Scriptless SP outputs do not affect input lock requirements.
    let tx = psbt
        .to_bitcoin_psbt_with_empty_sp_outputs()
        .map_err(invalid)?
        .unsigned_tx;
    Locks { absolute, relative }
        .validate_transaction(&tx, selected.input_index as usize)
        .map_err(invalid)
}

fn finalize(mut psbt: PsbtV2, inputs: &[RustSelectedInput]) -> Result<CoordinationResult, Failure> {
    verify_musig_contributions(&psbt, inputs)?;
    verify_sp_contributions(&psbt)?;
    bip375::verify_signed(&psbt).map_err(invalid)?;
    let mut v0 = psbt.clone().into_bitcoin_psbt().map_err(invalid)?;
    for policy in policies(inputs)? {
        aggregate_partial_sigs(&mut v0, &policy).map_err(invalid)?;
    }
    v0.finalize_mut(&Secp256k1::verification_only())
        .map_err(|errors| {
            invalid(
                errors
                    .iter()
                    .map(ToString::to_string)
                    .collect::<Vec<_>>()
                    .join("; "),
            )
        })?;
    let tx = v0.clone().extract_tx().map_err(invalid)?;
    for (input, finalized) in psbt.inputs.iter_mut().zip(v0.inputs) {
        input.psbt = finalized;
    }
    let mut completed = success(&psbt, SigningProgress::Complete)?;
    completed.tx_hex = serialize_hex(&tx);
    completed.txid = tx.compute_txid().to_string();
    Ok(completed)
}

pub fn transaction_finalize(
    network: Network,
    psbt: &str,
    inputs: Vec<RustSelectedInput>,
    session_id: &str,
) -> CoordinationResult {
    result((|| {
        let psbt = decode(psbt)?;
        validate_inputs(network, &psbt, &inputs)?;
        validate_session_id(&psbt, session_id)?;
        finalize(psbt, &inputs)
    })())
}

pub fn coordination_prepare(
    network: Network,
    psbt: &str,
    inputs: Vec<RustSelectedInput>,
    stage: SigningStage,
) -> CoordinationResult {
    result((|| {
        let psbt = decode(psbt)?;
        validate_inputs(network, &psbt, &inputs)?;
        prepare_stage(psbt, &inputs, stage)
    })())
}

fn prepare_stage(
    mut psbt: PsbtV2,
    inputs: &[RustSelectedInput],
    stage: SigningStage,
) -> Result<CoordinationResult, Failure> {
    match stage {
        SigningStage::SpContributions => return Err((
            SpendError::Unsupported,
            "independent SP share/proof production is not exposed by the current signing routes"
                .into(),
        )),
        SigningStage::VerifySpContributions => {
            return success(&psbt, verify_sp_contributions(&psbt)?)
        }
        SigningStage::CompleteSpOutputs => {
            verify_sp_contributions(&psbt)?;
            bip375::complete_output_scripts(&mut psbt).map_err(invalid)?;
            psbt.tx_modifiable = Some(TxModifiable::none());
            return success(&psbt, SigningProgress::StageComplete);
        }
        SigningStage::Release => return Err(invalid("release must use the retirement operation")),
        SigningStage::Signatures
        | SigningStage::MusigNonces
        | SigningStage::MusigSignatures
        | SigningStage::SpCompleteAndSign => {}
        _ => return Err(invalid("unknown signing stage")),
    }
    let sp_owners = inputs
        .iter()
        .filter(|input| !input.variant.sp_spend_key.is_empty())
        .map(|input| &input.variant.sp_spend_key)
        .collect::<BTreeSet<_>>();
    if sp_owners.len() > 1 {
        return Err((
            SpendError::Unsupported,
            "current signing routes cannot coordinate multiple SP owners".into(),
        ));
    }
    if stage == SigningStage::SpCompleteAndSign {
        return prepare_combined_sp(psbt, inputs);
    }
    prepare_signatures(psbt, inputs, stage)
}

fn prepare_combined_sp(
    mut psbt: PsbtV2,
    inputs: &[RustSelectedInput],
) -> Result<CoordinationResult, Failure> {
    verify_sp_contributions(&psbt)?;
    let fingerprints = crate::keys::psbt::required_fingerprints(&psbt).map_err(invalid)?;
    let empty_outputs = psbt
        .to_bitcoin_psbt_with_empty_sp_outputs()
        .map_err(invalid)?;
    let has_musig = policies(inputs)?
        .iter()
        .map(|policy| musig_status(&empty_outputs, policy).map_err(invalid))
        .collect::<Result<Vec<_>, _>>()?
        .iter()
        .any(|status| status.round != MusigRound::NotMusig);
    if fingerprints.len() != 1 || has_musig {
        return Err((SpendError::Unsupported, "combined SP completion/signing requires one identifiable signing key and no MuSig inputs".into()));
    }
    psbt.tx_modifiable = Some(TxModifiable::none());
    success(&psbt, SigningProgress::Partial)
}

fn prepare_signatures(
    mut psbt: PsbtV2,
    inputs: &[RustSelectedInput],
    stage: SigningStage,
) -> Result<CoordinationResult, Failure> {
    let mut v0 = psbt.clone().into_bitcoin_psbt().map_err(|e| {
        (
            SpendError::Unsupported,
            format!("complete SP outputs before ordinary/MuSig signing: {e}"),
        )
    })?;
    let rounds = policies(inputs)?
        .iter()
        .map(|policy| {
            musig_status(&v0, policy)
                .map(|status| status.round)
                .map_err(invalid)
        })
        .collect::<Result<Vec<_>, _>>()?;
    let has_musig = rounds.iter().any(|round| *round != MusigRound::NotMusig);
    match stage {
        SigningStage::Signatures if has_musig => {
            return Err(invalid(
                "MuSig inputs require explicit nonce/signature rounds",
            ))
        }
        SigningStage::MusigNonces if !has_musig || !rounds.contains(&MusigRound::Nonces) => {
            return Err(invalid("PSBT is not in the MuSig nonce round"))
        }
        SigningStage::MusigNonces
            if rounds
                .iter()
                .any(|round| matches!(round, MusigRound::PartialSigs | MusigRound::Ready)) =>
        {
            return Err((SpendError::Unsupported, "current signing routes cannot collect nonces while another policy is ready for signatures".into()));
        }
        SigningStage::MusigSignatures if !has_musig || rounds.contains(&MusigRound::Nonces) => {
            return Err(invalid(
                "all MuSig nonces are required before partial signatures",
            ))
        }
        _ => {}
    }
    if has_musig
        && musig_sessions::attempt_id(&v0.proprietary)
            .map_err(invalid)?
            .is_none()
    {
        if stage != SigningStage::MusigNonces
            || v0.inputs.iter().any(|input| {
                input.unknown.keys().any(|key| {
                    matches!(
                        key.type_value,
                        PSBT_IN_MUSIG2_PUB_NONCE | PSBT_IN_MUSIG2_PARTIAL_SIG
                    )
                })
            })
        {
            return Err((
                SpendError::LostSession,
                "MuSig progress has no attempt identity".into(),
            ));
        }
        musig_sessions::start_attempt(&mut v0).map_err(invalid)?;
        psbt.proprietary = v0.proprietary;
    }
    psbt.tx_modifiable = Some(TxModifiable::none());
    verify_sp_contributions(&psbt)?;
    bip375::verify_signed(&psbt).map_err(signature_error)?;
    verify_signatures(&psbt)?;
    verify_musig_contributions(&psbt, inputs)?;
    success(&psbt, SigningProgress::Partial)
}

pub fn coordination_accept(
    original: &str,
    returned: &str,
    inputs: Vec<RustSelectedInput>,
    stage: SigningStage,
) -> CoordinationResult {
    result((|| {
        let before = decode(original)?;
        let after = decode(returned)?;
        if !matches!(
            stage,
            SigningStage::Signatures
                | SigningStage::MusigNonces
                | SigningStage::MusigSignatures
                | SigningStage::SpCompleteAndSign
        ) {
            return Err(invalid("this stage does not accept a signer response"));
        }
        let network = inputs
            .first()
            .ok_or_else(|| invalid("missing input selections"))?
            .network;
        validate_inputs(network, &before, &inputs)?;
        validate_inputs(network, &after, &inputs)?;
        preserve_signatures(&before, &after)?;
        let merged = merge_signed_psbt(original, returned);
        if !merged.is_ok {
            return Err((SpendError::Integrity, merged.error));
        }
        let canonical = decode(&merged.psbt_v2_hex)?;
        if canonical != after {
            return Err((
                SpendError::Integrity,
                "signer changed fields outside the permitted additive signature/nonce maps".into(),
            ));
        }
        if before == after {
            return Err(invalid("signer made no progress"));
        }
        verify_signatures(&after)?;
        verify_musig_contributions(&after, &inputs)?;
        let forbidden = if stage == SigningStage::MusigNonces {
            PSBT_IN_MUSIG2_PARTIAL_SIG
        } else {
            PSBT_IN_MUSIG2_PUB_NONCE
        };
        for (old, new) in before.inputs.iter().zip(&after.inputs) {
            if !old
                .psbt
                .unknown
                .iter()
                .filter(|(key, _)| key.type_value == forbidden)
                .eq(new
                    .psbt
                    .unknown
                    .iter()
                    .filter(|(key, _)| key.type_value == forbidden))
            {
                return Err((
                    SpendError::Integrity,
                    "signer changed MuSig fields belonging to another round".into(),
                ));
            }
        }
        if stage == SigningStage::MusigNonces {
            let v0 = after.clone().into_bitcoin_psbt().map_err(invalid)?;
            let complete = policies(&inputs)?
                .iter()
                .map(|policy| musig_status(&v0, policy).map_err(invalid))
                .collect::<Result<Vec<_>, _>>()?
                .iter()
                .all(|status| status.round != MusigRound::Nonces);
            return success(
                &after,
                if complete {
                    SigningProgress::StageComplete
                } else {
                    SigningProgress::Partial
                },
            );
        }
        success(&after, signing_progress(&after, &inputs)?)
    })())
}

fn verify_sp_contributions(psbt: &PsbtV2) -> Result<SigningProgress, Failure> {
    use bwk::bwk_tx::bwk_psbt::sp::{sp_global_ecdh_share_v2, sp_input_dleq, sp_input_ecdh_share};
    bip375::validate_share_maps(psbt).map_err(invalid)?;
    bip375::validate_input_share_pairs(psbt).map_err(invalid)?;
    bip375::validate_input_eligibility(psbt, true).map_err(invalid)?;
    let mut complete = true;
    for scan_key in bip375::scan_keys(psbt).map_err(invalid)? {
        let global = sp_global_ecdh_share_v2(psbt, scan_key)
            .map_err(invalid)?
            .is_some();
        if global {
            bip375::validate_global_share(psbt, scan_key)
                .map_err(|e| (SpendError::Integrity, e.to_string()))?;
        }
        for input in &psbt.inputs {
            match (
                bip375::eligible_input_pubkey(input).map_err(invalid)?,
                sp_input_ecdh_share(&input.psbt, scan_key).map_err(invalid)?,
                sp_input_dleq(&input.psbt, scan_key).map_err(invalid)?,
            ) {
                (Some(pubkey), Some(share), Some(proof)) => {
                    bip375::verify_proof(pubkey, scan_key, share, proof)
                        .map_err(|e| (SpendError::Integrity, e.to_string()))?
                }
                (Some(_), None, None) => complete &= global,
                (None, None, None) => {}
                _ => return Err(invalid("SP contribution is missing its share or proof")),
            }
        }
    }
    Ok(if complete {
        SigningProgress::StageComplete
    } else {
        SigningProgress::Partial
    })
}

fn preserve_signatures(before: &PsbtV2, after: &PsbtV2) -> Result<(), Failure> {
    for (before, after) in before.inputs.iter().zip(&after.inputs) {
        if before
            .psbt
            .partial_sigs
            .iter()
            .any(|(key, sig)| after.psbt.partial_sigs.get(key) != Some(sig))
            || before
                .psbt
                .tap_script_sigs
                .iter()
                .any(|(key, sig)| after.psbt.tap_script_sigs.get(key) != Some(sig))
            || before
                .psbt
                .tap_key_sig
                .is_some_and(|sig| after.psbt.tap_key_sig != Some(sig))
        {
            return Err((
                SpendError::Integrity,
                "signer replaced or removed an existing signature".into(),
            ));
        }
    }
    Ok(())
}

fn signature_error(error: impl ToString) -> Failure {
    (SpendError::Integrity, error.to_string())
}

fn script_mentions_key(script: &bitcoin::Script, key: &[u8]) -> bool {
    let hash = hash160::Hash::hash(key);
    script.instructions().any(|instruction| match instruction {
        Ok(bitcoin::script::Instruction::PushBytes(bytes)) => {
            bytes.as_bytes() == key || bytes.as_bytes() == hash.as_byte_array()
        }
        _ => false,
    })
}

fn verify_signatures(psbt: &PsbtV2) -> Result<(), Failure> {
    let v0 = psbt.clone().into_bitcoin_psbt().map_err(signature_error)?;
    let prevouts = v0
        .inputs
        .iter()
        .map(|input| {
            input
                .witness_utxo
                .clone()
                .ok_or_else(|| signature_error("missing witness UTXO"))
        })
        .collect::<Result<Vec<_>, _>>()?;
    let secp = Secp256k1::verification_only();
    let mut cache = SighashCache::new(&v0.unsigned_tx);
    for (index, input) in v0.inputs.iter().enumerate() {
        for (key, sig) in &input.partial_sigs {
            let script = input
                .witness_script
                .as_ref()
                .or(input.redeem_script.as_ref())
                .unwrap_or(&prevouts[index].script_pubkey);
            if !script_mentions_key(script, &key.to_bytes()) {
                return Err(signature_error(
                    "signature key is not in the selected input script",
                ));
            }
            let (message, sighash) = v0
                .sighash_ecdsa(index, &mut cache)
                .map_err(signature_error)?;
            if sig.sighash_type != sighash {
                return Err(signature_error(
                    "signature sighash differs from the requested sighash",
                ));
            }
            secp.verify_ecdsa(&message, &sig.signature, &key.inner)
                .map_err(signature_error)?;
        }
        if let Some(sig) = input.tap_key_sig {
            check_tap_sighash(input, sig.sighash_type)?;
            let script = &prevouts[index].script_pubkey;
            if !script.is_p2tr() {
                return Err(signature_error("key-path signature on a non-Taproot input"));
            }
            let key = bitcoin::XOnlyPublicKey::from_slice(&script.as_bytes()[2..])
                .map_err(signature_error)?;
            let digest = cache
                .taproot_key_spend_signature_hash(
                    index,
                    &Prevouts::All(&prevouts),
                    sig.sighash_type,
                )
                .map_err(signature_error)?;
            secp.verify_schnorr(
                &sig.signature,
                &bitcoin::secp256k1::Message::from_digest(digest.to_byte_array()),
                &key,
            )
            .map_err(signature_error)?;
        }
        for ((key, leaf), sig) in &input.tap_script_sigs {
            check_tap_sighash(input, sig.sighash_type)?;
            let script = input
                .tap_scripts
                .values()
                .find(|(script, version)| {
                    bitcoin::taproot::TapLeafHash::from_script(script, *version) == *leaf
                })
                .map(|(script, _)| script)
                .ok_or_else(|| signature_error("signature names an unselected Taproot leaf"))?;
            if !script_mentions_key(script, &key.serialize()) {
                return Err(signature_error("signature key is not in the selected leaf"));
            }
            let digest = cache
                .taproot_script_spend_signature_hash(
                    index,
                    &Prevouts::All(&prevouts),
                    *leaf,
                    sig.sighash_type,
                )
                .map_err(signature_error)?;
            secp.verify_schnorr(
                &sig.signature,
                &bitcoin::secp256k1::Message::from_digest(digest.to_byte_array()),
                key,
            )
            .map_err(signature_error)?;
        }
    }
    Ok(())
}

fn check_tap_sighash(
    input: &bitcoin::psbt::Input,
    actual: bitcoin::TapSighashType,
) -> Result<(), Failure> {
    match input.sighash_type {
        Some(expected) if expected.taproot_hash_ty().map_err(signature_error)? == actual => Ok(()),
        None if matches!(
            actual,
            bitcoin::TapSighashType::Default | bitcoin::TapSighashType::All
        ) =>
        {
            Ok(())
        }
        _ => Err(signature_error(
            "signature sighash differs from the requested sighash",
        )),
    }
}

fn verify_musig_contributions(psbt: &PsbtV2, inputs: &[RustSelectedInput]) -> Result<(), Failure> {
    let psbt = psbt.clone().into_bitcoin_psbt().map_err(signature_error)?;
    for input in &psbt.inputs {
        for nonce in musig_input_pub_nonces(input).map_err(signature_error)? {
            // Singleton NonceAgg parses both points without needing other signers.
            nonce_agg(&RustBitcoin, &[nonce.nonce]).map_err(signature_error)?;
        }
    }
    let mut verified = BTreeSet::new();
    for policy in policies(inputs)? {
        for info in slot_infos(&psbt, &policy).map_err(signature_error)? {
            if verify_partial(&psbt, &info)? {
                verified.insert(info.slot);
            }
        }
    }
    for (index, input) in psbt.inputs.iter().enumerate() {
        for sig in musig_input_partial_sigs(input).map_err(signature_error)? {
            if !verified.contains(&bwk::bwk_sign::musig::MusigSlot {
                input_index: index,
                participant: sig.participant,
                aggregate: sig.aggregate,
                leaf_hash: sig.leaf_hash,
            }) {
                return Err(signature_error(
                    "partial signature does not belong to a selected policy slot",
                ));
            }
        }
    }
    Ok(())
}

fn verify_partial(psbt: &bitcoin::Psbt, info: &SlotInfo) -> Result<bool, Failure> {
    let slot = info.slot;
    let input = &psbt.inputs[slot.input_index];
    let Some(signature) =
        musig_input_partial_sig(input, slot.participant, slot.aggregate, slot.leaf_hash)
            .map_err(signature_error)?
    else {
        return Ok(false);
    };
    let nonces = info
        .agg
        .participants()
        .iter()
        .map(|participant| {
            musig_input_pub_nonce(input, *participant, slot.aggregate, slot.leaf_hash)
                .map_err(signature_error)?
                .ok_or_else(|| {
                    signature_error("partial signature requires every participant's public nonce")
                })
        })
        .collect::<Result<Vec<_>, _>>()?;
    let aggregate_nonce = nonce_agg(&RustBitcoin, &nonces).map_err(signature_error)?;
    let key_agg = match slot.leaf_hash {
        None => info.agg.key_path_key_agg(info.merkle_root),
        Some(_) => info.agg.key_agg(),
    }
    .map_err(signature_error)?;
    let own_nonce = musig_input_pub_nonce(input, slot.participant, slot.aggregate, slot.leaf_hash)
        .map_err(signature_error)?
        .ok_or_else(|| signature_error("partial signature has no public nonce"))?;
    let sighash = slot_sighash(psbt, &slot).map_err(signature_error)?;
    if !partial_verify(
        &RustBitcoin,
        &signature,
        &own_nonce,
        &slot.participant.serialize(),
        &key_agg,
        &aggregate_nonce,
        &sighash,
    ) {
        return Err(signature_error("invalid MuSig partial signature"));
    }
    Ok(true)
}

fn signing_progress(
    psbt: &PsbtV2,
    inputs: &[RustSelectedInput],
) -> Result<SigningProgress, Failure> {
    verify_sp_contributions(psbt)?;
    bip375::verify_signed(psbt).map_err(|error| (SpendError::Integrity, error.to_string()))?;
    let mut v0 = psbt.clone().into_bitcoin_psbt().map_err(invalid)?;
    for policy in policies(inputs)? {
        aggregate_partial_sigs(&mut v0, &policy)
            .map_err(|error| (SpendError::Integrity, error.to_string()))?;
    }
    match v0.finalize_mut(&Secp256k1::verification_only()) {
        Ok(()) => {
            v0.extract_tx().map_err(signature_error)?;
            Ok(SigningProgress::Complete)
        }
        Err(errors) if errors.iter().all(missing_satisfaction) => Ok(SigningProgress::Partial),
        Err(errors) => Err((
            SpendError::Integrity,
            errors
                .iter()
                .map(ToString::to_string)
                .collect::<Vec<_>>()
                .join("; "),
        )),
    }
}

fn missing_satisfaction(error: &PsbtError) -> bool {
    matches!(
        error,
        PsbtError::InputError(
            InputError::CouldNotSatisfyTr
                | InputError::MissingPubkey
                | InputError::MiniscriptError(
                    MiniscriptError::MissingSig(_) | MiniscriptError::CouldNotSatisfy
                ),
            _
        )
    )
}

pub fn coordination_release(
    network: Network,
    psbt: &str,
    inputs: Vec<RustSelectedInput>,
    session_id: &str,
) -> CoordinationResult {
    result((|| {
        let decoded = decode(psbt)?;
        validate_inputs(network, &decoded, &inputs)?;
        validate_session_id(&decoded, session_id)?;
        let mut next = psbt.to_string();
        for policy in policies(&inputs)? {
            let v0 = decoded.clone().into_bitcoin_psbt().map_err(invalid)?;
            if musig_status(&v0, &policy).map_err(invalid)?.round == MusigRound::NotMusig {
                continue;
            }
            let retired = musig_session_restart(network, &policy.to_string(), psbt, &next);
            if !retired.is_ok {
                return Err((SpendError::LostSession, retired.error));
            }
            next = retired.psbt_v2_hex;
        }
        success(&decode(&next)?, SigningProgress::StageComplete)
    })())
}

fn validate_session_id(psbt: &PsbtV2, expected: &str) -> Result<(), Failure> {
    let identity = musig_sessions::attempt_id(&psbt.proprietary)
        .map_err(invalid)?
        .map(hex::encode)
        .unwrap_or_default();
    if !expected.is_empty() && identity != expected {
        return Err(signature_error("session identity does not match the PSBT"));
    }
    Ok(())
}

pub fn transaction_broadcast(network: Network, endpoint: &str, tx_hex: &str) -> CoordinationResult {
    result((|| {
        let tx: bitcoin::Transaction = deserialize_hex(tx_hex).map_err(invalid)?;
        if tx.input.is_empty() || tx.output.is_empty() {
            return Err(invalid("empty transaction"));
        }
        network_broadcast(network, endpoint, &tx)?;
        Ok(CoordinationResult {
            is_ok: true,
            error: String::new(),
            kind: SpendError::Other,
            psbt_v2_hex: String::new(),
            progress: SigningProgress::Complete,
            tx_hex: tx_hex.into(),
            txid: tx.compute_txid().to_string(),
            session_id: String::new(),
        })
    })())
}

fn network_broadcast(
    network: Network,
    endpoint: &str,
    tx: &bitcoin::Transaction,
) -> Result<(), Failure> {
    use bwk::bwk_electrum::{
        electrum::{request::Request, response::Response},
        raw_client::Client,
        url::{parse_electrum_url, ElectrumScheme},
    };
    let (host, port, scheme) = parse_electrum_url(endpoint).map_err(invalid)?;
    let (Some(host), Some(port)) = (host, port) else {
        return Err(invalid("invalid Electrum endpoint"));
    };
    let mut client = Client::new_ssl_maybe(&host, port, matches!(scheme, ElectrumScheme::Ssl))
        .certificate_check(crate::config::certificate_check(true))
        .map_err(invalid)?;
    client
        .try_connect(Some(Duration::from_secs(10)))
        .map_err(invalid)?;
    let request = Request::header(0);
    client.try_send(&request).map_err(invalid)?;
    let responses = client
        .recv(&HashMap::from([(request.id, request.clone())]))
        .map_err(invalid)?;
    let raw_header = responses
        .into_iter()
        .find_map(|response| match response {
            Response::Header(header) if header.id == request.id => Some(header.raw_header),
            _ => None,
        })
        .ok_or_else(|| invalid("Electrum did not return its genesis header"))?;
    let header: bitcoin::block::Header = deserialize_hex(&raw_header).map_err(invalid)?;
    if header.block_hash()
        != bitcoin::blockdata::constants::genesis_block(bitcoin::Network::from(network))
            .block_hash()
    {
        return Err(invalid("Electrum endpoint network mismatch"));
    }
    let request = Request::tx_broadcast(serialize_hex(tx));
    client.try_send(&request).map_err(invalid)?;
    let responses = client
        .recv(&HashMap::from([(request.id, request.clone())]))
        .map_err(invalid)?;
    for response in responses {
        match response {
            Response::TxBroadcast(answer)
                if answer.id == request.id && answer.txid == tx.compute_txid() =>
            {
                return Ok(())
            }
            Response::Error(error) if error.id == request.id => {
                return Err((SpendError::Other, format!("broadcast rejected: {error:?}")))
            }
            _ => {}
        }
    }
    Err((
        SpendError::Other,
        "missing or mismatched broadcast acknowledgement".into(),
    ))
}
