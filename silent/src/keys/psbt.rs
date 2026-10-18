use std::collections::{BTreeMap, BTreeSet};

use bitcoin::{
    bip32::Fingerprint,
    hashes::{hash160, Hash},
    secp256k1::{All, PublicKey, Scalar, Secp256k1},
    Psbt, ScriptBuf,
};
use bwk_sp::{
    account::bip375,
    bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor,
    bwk_tx::bwk_psbt::{
        musig::{
            musig_input_partial_sigs, musig_input_participants_all, musig_input_pub_nonces,
            remove_musig_input_partial_sigs, remove_musig_input_pub_nonces,
            set_musig_input_participants, Error as MusigFieldError, PSBT_IN_MUSIG2_PARTIAL_SIG,
            PSBT_IN_MUSIG2_PUB_NONCE,
        },
        sp::{
            sp_input_tweak, sp_v0_output, PSBT_GLOBAL_SP_DLEQ, PSBT_GLOBAL_SP_ECDH_SHARE,
            PSBT_IN_SP_DLEQ, PSBT_IN_SP_ECDH_SHARE, PSBT_IN_SP_SPEND_BIP32_DERIVATION,
            PSBT_IN_SP_TWEAK, PSBT_OUT_SP_V0_INFO, PSBT_OUT_SP_V0_LABEL,
        },
        taproot::is_partial_tap_tree,
        Error as PsbtV2Error, PsbtV2,
    },
};

use crate::keys::error::Error;

const FINGERPRINT_LEN: usize = 4;
const MUSIG_INPUT_TYPES: [u8; 2] = [PSBT_IN_MUSIG2_PUB_NONCE, PSBT_IN_MUSIG2_PARTIAL_SIG];

/// What a PSBT requires to be fully signed, worked out from the PSBT alone.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PsbtArtifacts {
    /// Every fingerprint that has to sign, in a deterministic order.
    pub required: BTreeSet<Fingerprint>,
    /// The fingerprint of the sp() key that must sign first, when resolved.
    pub sp_fingerprint: Option<Fingerprint>,
    /// At least one input is a silent payment input.
    pub sp_inputs: bool,
    /// At least one output is a silent payment output.
    pub sp_outputs: bool,
    /// At least one output script is still underived, so no signature over these
    /// bytes can survive.
    pub sp_outputs_pending: bool,
    /// Fingerprints whose share for the current MuSig2 round is already in the PSBT.
    pub musig_done: BTreeSet<Fingerprint>,
}

pub fn parse_v2(bytes: &[u8]) -> Result<PsbtV2, Error> {
    PsbtV2::deserialize(bytes).map_err(|e| Error::PsbtParse(e.to_string()))
}

/// Result of scanning a PSBT's inputs and outputs for required signers and SP
/// artifacts, without requiring an SP descriptor.
struct Scan {
    required: BTreeSet<Fingerprint>,
    sp_inputs: bool,
    sp_outputs: bool,
    sp_outputs_pending: bool,
}

fn scan(psbt: &PsbtV2) -> Result<Scan, Error> {
    let mut required = BTreeSet::new();
    let mut synthetic = BTreeSet::new();
    let mut sp_inputs = false;

    for input in &psbt.inputs {
        for entry in musig_input_participants_all(&input.psbt).map_err(musig_field_error)? {
            synthetic.insert(synthetic_fingerprint(&entry.aggregate));
        }
        for (fingerprint, _) in input.psbt.bip32_derivation.values() {
            required.insert(*fingerprint);
        }
        // A PSBT restricted to one leaf keeps every key origin (bwk_descriptor::restrict),
        // the keys outside the leaf with no leaf hash: only the leaf's keys sign it.
        let restricted = is_partial_tap_tree(&input.psbt);
        for (leaf_hashes, (fingerprint, _)) in input.psbt.tap_key_origins.values() {
            if restricted && leaf_hashes.is_empty() {
                continue;
            }
            required.insert(*fingerprint);
        }

        let mut input_has_sp_spend_derivation = false;
        for (key, value) in &input.psbt.unknown {
            if key.type_value != PSBT_IN_SP_SPEND_BIP32_DERIVATION {
                continue;
            }
            input_has_sp_spend_derivation = true;
            let fingerprint_bytes: [u8; FINGERPRINT_LEN] = value
                .get(..FINGERPRINT_LEN)
                .and_then(|slice| slice.try_into().ok())
                .ok_or_else(|| {
                    Error::SpField(format!(
                        "sp spend derivation value is {} bytes, expected at least {FINGERPRINT_LEN}",
                        value.len()
                    ))
                })?;
            required.insert(Fingerprint::from(fingerprint_bytes));
        }

        let input_has_sp_tweak = input
            .psbt
            .unknown
            .keys()
            .any(|key| key.type_value == PSBT_IN_SP_TWEAK);
        if input_has_sp_tweak || input_has_sp_spend_derivation {
            sp_inputs = true;
        }
    }

    // A musig aggregate's tap_key_origins entry names its BIP-328 synthetic xpub,
    // which no device holds: only its cosigners sign.
    required.retain(|fingerprint| !synthetic.contains(fingerprint));

    let mut sp_outputs = false;
    let mut sp_outputs_pending = false;
    for output in &psbt.outputs {
        if sp_v0_output(&output.psbt)
            .map_err(|e| Error::SpField(e.to_string()))?
            .is_some()
        {
            sp_outputs = true;
        }
        if output.script_pubkey.is_none() {
            sp_outputs_pending = true;
        }
    }

    Ok(Scan {
        required,
        sp_inputs,
        sp_outputs,
        sp_outputs_pending,
    })
}

/// Whether `psbt` carries silent payment artifacts (SP output info, input tweaks,
/// or ECDH shares), read directly off the PSBT's own fields. No descriptor is
/// needed since this only detects artifacts, it does not resolve the signer
/// fingerprint behind them.
pub fn has_sp_artifacts(psbt: &PsbtV2) -> Result<bool, Error> {
    let scan = scan(psbt)?;
    Ok(scan.sp_inputs || scan.sp_outputs)
}

/// Every fingerprint the PSBT itself names: bip32_derivation, tap_key_origins and
/// the silent payment spend derivation. Unlike `inspect`, this never resolves an sp()
/// signer, so it cannot add the fingerprint behind an SP tweak or SP output that
/// carries no spend derivation yet.
pub fn required_fingerprints(psbt: &PsbtV2) -> Result<BTreeSet<Fingerprint>, Error> {
    Ok(scan(psbt)?.required)
}

/// One SP input's spend derivation: the spend public key named by the derivation's
/// own raw key, and the fingerprint claimed by its value.
struct SpendDerivation {
    fingerprint: Fingerprint,
    spend_pubkey: PublicKey,
}

/// Every SP spend derivation found on any input. `scan` only reads the fingerprint
/// out of the value; this walks the same `unknown` entries again to also recover
/// the spend public key carried in the raw key's own bytes.
fn spend_derivations(psbt: &PsbtV2) -> Result<Vec<SpendDerivation>, Error> {
    let mut found = Vec::new();
    for input in &psbt.inputs {
        for (raw_key, value) in &input.psbt.unknown {
            if raw_key.type_value != PSBT_IN_SP_SPEND_BIP32_DERIVATION {
                continue;
            }
            let spend_pubkey = PublicKey::from_slice(&raw_key.key).map_err(|e| {
                Error::SpField(format!("sp spend derivation key is not a public key: {e}"))
            })?;
            let fingerprint_bytes: [u8; FINGERPRINT_LEN] = value
                .get(..FINGERPRINT_LEN)
                .and_then(|slice| slice.try_into().ok())
                .ok_or_else(|| {
                    Error::SpField(format!(
                        "sp spend derivation value is {} bytes, expected at least {FINGERPRINT_LEN}",
                        value.len()
                    ))
                })?;
            found.push(SpendDerivation {
                fingerprint: Fingerprint::from(fingerprint_bytes),
                spend_pubkey,
            });
        }
    }
    Ok(found)
}

/// The registered descriptor whose origin fingerprint is `fingerprint` and whose
/// spend public key is `spend_pubkey`, if any.
fn find_matching_descriptor<'a>(
    sp_descriptors: &'a [SpDescriptor],
    fingerprint: Fingerprint,
    spend_pubkey: PublicKey,
    secp: &Secp256k1<All>,
) -> Result<Option<&'a SpDescriptor>, Error> {
    for descriptor in sp_descriptors {
        if descriptor.fingerprint() != Some(fingerprint) {
            continue;
        }
        let candidate_pubkey = descriptor
            .spend_public_key(secp)
            .map_err(|e| Error::DescriptorParse(e.to_string()))?;
        if candidate_pubkey == spend_pubkey {
            return Ok(Some(descriptor));
        }
    }
    Ok(None)
}

/// A P2TR script is `OP_1 <push 32 bytes> <32-byte x-only key>`: the key starts
/// right after the 1-byte opcode and 1-byte push-length prefix.
const P2TR_KEY_OFFSET: usize = 2;
const P2TR_KEY_LEN: usize = 32;

/// The x-only output key of `script_pubkey`, when it is a P2TR script.
fn p2tr_output_key(script_pubkey: &ScriptBuf) -> Option<[u8; 32]> {
    if !script_pubkey.is_p2tr() {
        return None;
    }
    script_pubkey
        .as_bytes()
        .get(P2TR_KEY_OFFSET..P2TR_KEY_OFFSET + P2TR_KEY_LEN)?
        .try_into()
        .ok()
}

/// Whether `input`'s SP tweak, if it has one, resolves `spend_pubkey` to its P2TR
/// prevout's output key: `spend_pubkey + t*G` must equal the prevout's x-only key.
/// `None` when the input carries no tweak at all.
fn tweak_matches(
    input: &bitcoin::psbt::Input,
    spend_pubkey: PublicKey,
    secp: &Secp256k1<All>,
) -> Result<Option<bool>, Error> {
    let Some(tweak) = sp_input_tweak(input).map_err(|e| Error::SpField(e.to_string()))? else {
        return Ok(None);
    };
    let scalar = Scalar::from_be_bytes(tweak)
        .map_err(|_| Error::SpField("sp input tweak is out of range".to_string()))?;
    let tweaked = spend_pubkey
        .add_exp_tweak(secp, &scalar)
        .map_err(|e| Error::SpField(format!("sp input tweak addition failed: {e}")))?;
    let (tweaked_x, _) = tweaked.x_only_public_key();

    let matches = input
        .witness_utxo
        .as_ref()
        .and_then(|utxo| p2tr_output_key(&utxo.script_pubkey))
        .is_some_and(|prevout_x| prevout_x == tweaked_x.serialize());
    Ok(Some(matches))
}

/// Resolution for a PSBT whose inputs carry SP tweaks but no spend derivation at
/// all: every registered descriptor is checked against every tweaked input, and
/// exactly one must match all of them.
fn resolve_by_tweak_only(
    psbt: &PsbtV2,
    sp_descriptors: &[SpDescriptor],
    secp: &Secp256k1<All>,
) -> Result<Option<Fingerprint>, Error> {
    let mut matched = BTreeSet::new();
    for descriptor in sp_descriptors {
        let spend_pubkey = descriptor
            .spend_public_key(secp)
            .map_err(|e| Error::DescriptorParse(e.to_string()))?;

        let mut saw_tweak = false;
        let mut all_match = true;
        for input in &psbt.inputs {
            match tweak_matches(&input.psbt, spend_pubkey, secp)? {
                None => {}
                Some(true) => saw_tweak = true,
                Some(false) => {
                    all_match = false;
                    break;
                }
            }
        }
        if saw_tweak && all_match {
            if let Some(fingerprint) = descriptor.fingerprint() {
                matched.insert(fingerprint);
            }
        }
    }

    let mut matched = matched.into_iter();
    match (matched.next(), matched.next()) {
        // No spend derivation names a fingerprint to report here, so there is
        // no single key to point at.
        (None, _) => Err(Error::SpKeyUnknown(None)),
        (Some(fingerprint), None) => Ok(Some(fingerprint)),
        (Some(_), Some(_)) => Err(Error::SpInputsDisagree),
    }
}

/// Resolves the fingerprint of the sp() key that must sign `psbt` first, from the
/// PSBT's own SP fields cross-checked against `sp_descriptors`:
/// 1. every SP input's spend derivation must name the same fingerprint, else
///    `Error::SpInputsDisagree`;
/// 2. that fingerprint must match a registered descriptor whose spend public key is
///    the one named in the derivation, else `Error::SpKeyUnknown`;
/// 3. every input carrying an SP tweak must resolve that descriptor's spend key to
///    its own P2TR prevout, else `Error::SpKeyUnknown`;
/// 4. with no spend derivation at all but at least one SP tweak, every registered
///    descriptor is tried against every tweaked input instead, and exactly one must
///    match, else `Error::SpKeyUnknown` (none matched) or `Error::SpInputsDisagree`
///    (more than one did);
/// 5. a pending SP output with no SP input data anywhere is refused as
///    `Error::SpOutputsUnresolvable` rather than guessed;
/// 6. a PSBT with none of the above resolves to no SP fingerprint.
fn resolve_sp_fingerprint(
    psbt: &PsbtV2,
    sp_descriptors: &[SpDescriptor],
    secp: &Secp256k1<All>,
    sp_outputs_pending: bool,
) -> Result<Option<Fingerprint>, Error> {
    let derivations = spend_derivations(psbt)?;

    if !derivations.is_empty() {
        let fingerprints: BTreeSet<Fingerprint> =
            derivations.iter().map(|d| d.fingerprint).collect();
        if fingerprints.len() > 1 {
            return Err(Error::SpInputsDisagree);
        }
        let fingerprint = derivations[0].fingerprint;
        let spend_pubkey = derivations[0].spend_pubkey;

        let descriptor = find_matching_descriptor(sp_descriptors, fingerprint, spend_pubkey, secp)?
            .ok_or(Error::SpKeyUnknown(Some(fingerprint)))?;
        let resolved_spend_pubkey = descriptor
            .spend_public_key(secp)
            .map_err(|e| Error::DescriptorParse(e.to_string()))?;

        for input in &psbt.inputs {
            if tweak_matches(&input.psbt, resolved_spend_pubkey, secp)? == Some(false) {
                return Err(Error::SpKeyUnknown(Some(fingerprint)));
            }
        }
        return Ok(Some(fingerprint));
    }

    let has_tweak = psbt.inputs.iter().any(|input| {
        input
            .psbt
            .unknown
            .keys()
            .any(|key| key.type_value == PSBT_IN_SP_TWEAK)
    });
    if has_tweak {
        return resolve_by_tweak_only(psbt, sp_descriptors, secp);
    }

    if sp_outputs_pending {
        return resolve_by_input_owner(psbt, sp_descriptors);
    }

    Ok(None)
}

/// No silent payment input names the signer that must derive the pending outputs:
/// the key that owns every input does (BIP375: the shares come from the inputs'
/// keys), for instance an account spending its own sub-account coins to its silent
/// payment change. It must hold a registered sp() descriptor to sign as one. Inputs
/// of several keys need each to contribute its share, which is not supported.
fn resolve_by_input_owner(
    psbt: &PsbtV2,
    sp_descriptors: &[SpDescriptor],
) -> Result<Option<Fingerprint>, Error> {
    let owners = scan(psbt)?.required;
    let mut owners = owners.into_iter();
    let (Some(owner), None) = (owners.next(), owners.next()) else {
        return Err(Error::SpOutputsUnresolvable);
    };
    if !sp_descriptors
        .iter()
        .any(|descriptor| descriptor.fingerprint() == Some(owner))
    {
        return Err(Error::SpKeyUnknown(Some(owner)));
    }
    Ok(Some(owner))
}

pub fn inspect(
    psbt: &PsbtV2,
    sp_descriptors: &[SpDescriptor],
    secp: &Secp256k1<All>,
) -> Result<PsbtArtifacts, Error> {
    let Scan {
        mut required,
        sp_inputs,
        sp_outputs,
        sp_outputs_pending,
    } = scan(psbt)?;

    let sp_fingerprint = resolve_sp_fingerprint(psbt, sp_descriptors, secp, sp_outputs_pending)?;
    if let Some(fingerprint) = sp_fingerprint {
        required.insert(fingerprint);
    }

    Ok(PsbtArtifacts {
        required,
        sp_fingerprint,
        sp_inputs,
        sp_outputs,
        sp_outputs_pending,
        musig_done: musig_done_fingerprints(psbt)?,
    })
}

fn musig_field_error(e: MusigFieldError) -> Error {
    Error::PsbtParse(e.to_string())
}

/// The fingerprint of the BIP-328 synthetic xpub of `aggregate`.
fn synthetic_fingerprint(aggregate: &PublicKey) -> Fingerprint {
    let id = hash160::Hash::hash(&aggregate.serialize()).to_byte_array();
    Fingerprint::from([id[0], id[1], id[2], id[3]])
}

/// How many entries of one input name each participant.
type ParticipantCounts = BTreeMap<PublicKey, usize>;

fn count_participants(participants: impl IntoIterator<Item = PublicKey>) -> ParticipantCounts {
    let mut counts = ParticipantCounts::new();
    for participant in participants {
        *counts.entry(participant).or_default() += 1;
    }
    counts
}

/// Every participant of the input's 0x1a entries, once per entry listing it.
fn expected_participants(input: &bitcoin::psbt::Input) -> Result<ParticipantCounts, Error> {
    let entries = musig_input_participants_all(input).map_err(musig_field_error)?;
    Ok(count_participants(
        entries.into_iter().flat_map(|entry| entry.participants),
    ))
}

fn pub_nonce_counts(input: &bitcoin::psbt::Input) -> Result<ParticipantCounts, Error> {
    let nonces = musig_input_pub_nonces(input).map_err(musig_field_error)?;
    Ok(count_participants(
        nonces.into_iter().map(|nonce| nonce.participant),
    ))
}

fn partial_sig_counts(input: &bitcoin::psbt::Input) -> Result<ParticipantCounts, Error> {
    let sigs = musig_input_partial_sigs(input).map_err(musig_field_error)?;
    Ok(count_participants(
        sigs.into_iter().map(|sig| sig.participant),
    ))
}

/// Whether `got` holds at least `expected` entries for `participant`.
fn reached(got: &ParticipantCounts, participant: &PublicKey, expected: usize) -> bool {
    got.get(participant).copied().unwrap_or(0) >= expected
}

/// Every expected 0x1b present (expected participants read from 0x1a).
pub fn musig_nonces_complete(psbt: &PsbtV2) -> Result<bool, Error> {
    for input in &psbt.inputs {
        let expected = expected_participants(&input.psbt)?;
        let nonces = pub_nonce_counts(&input.psbt)?;
        if !expected
            .iter()
            .all(|(participant, count)| reached(&nonces, participant, *count))
        {
            return Ok(false);
        }
    }
    Ok(true)
}

/// In round 1 drops every 0x1b and 0x1c; whether anything was dropped.
pub fn strip_musig_round1(psbt: &mut PsbtV2) -> Result<bool, Error> {
    if musig_nonces_complete(psbt)? {
        return Ok(false);
    }
    let mut removed = 0;
    for input in &mut psbt.inputs {
        removed += remove_musig_input_pub_nonces(&mut input.psbt);
        removed += remove_musig_input_partial_sigs(&mut input.psbt);
    }
    Ok(removed > 0)
}

/// Fingerprints whose share for the current round is already in the PSBT: their
/// 0x1b while some nonce is missing, else their 0x1c. A fingerprint is done when
/// each of its participants has every entry its inputs expect.
pub fn musig_done_fingerprints(psbt: &PsbtV2) -> Result<BTreeSet<Fingerprint>, Error> {
    let nonces_complete = musig_nonces_complete(psbt)?;
    let mut done: BTreeMap<Fingerprint, bool> = BTreeMap::new();
    for input in &psbt.inputs {
        let expected = expected_participants(&input.psbt)?;
        let got = if nonces_complete {
            partial_sig_counts(&input.psbt)?
        } else {
            pub_nonce_counts(&input.psbt)?
        };
        for (participant, count) in &expected {
            let x_only = participant.x_only_public_key().0;
            let Some((_, (fingerprint, _))) = input.psbt.tap_key_origins.get(&x_only) else {
                log::debug!("musig participant {participant} has no tap_key_origins entry");
                continue;
            };
            *done.entry(*fingerprint).or_insert(true) &= reached(&got, participant, *count);
        }
    }
    Ok(done
        .into_iter()
        .filter(|(_, done)| *done)
        .map(|(fingerprint, _)| fingerprint)
        .collect())
}

pub fn inspect_bytes(
    bytes: &[u8],
    sp_descriptors: &[SpDescriptor],
    secp: &Secp256k1<All>,
) -> Result<PsbtArtifacts, Error> {
    let psbt = parse_v2(bytes)?;
    inspect(&psbt, sp_descriptors, secp)
}

/// Copies only signature fields (`partial_sigs`, `tap_key_sig`, `tap_script_sigs`)
/// and MuSig2 nonces / partial signatures from `signed` into `original`, adding
/// entries and never removing existing ones.
/// Refuses a `signed` PSBT for a different transaction, and an `original` PSBT with
/// an output the sp() key has not derived yet, since the txid to check `signed`
/// against does not exist until that output is derived. Returns how many inputs
/// gained at least one signature, MuSig2 nonce or partial signature.
pub fn merge_signatures(original: &mut PsbtV2, signed: &Psbt) -> Result<usize, Error> {
    let expected = match original.unsigned_tx() {
        Ok(tx) => tx.compute_txid(),
        Err(PsbtV2Error::MissingOutputScript) => return Err(Error::SpOutputsPending),
        Err(e) => return Err(Error::PsbtParse(e.to_string())),
    };
    let got = signed.unsigned_tx.compute_txid();
    if expected != got {
        return Err(Error::SignedPsbtMismatch { expected, got });
    }
    if original.inputs.len() != signed.inputs.len() {
        return Err(Error::PsbtParse(
            "signed psbt has a different number of inputs than the original".to_string(),
        ));
    }

    let mut touched = 0;
    for (input, signed_input) in original.inputs.iter_mut().zip(&signed.inputs) {
        if copy_signatures(&mut input.psbt, signed_input) {
            touched += 1;
        }
    }
    Ok(touched)
}

/// Takes the sp() signer's answer to an `original` whose silent payment outputs
/// have no script yet: the signer derived those scripts, so the txid to check the
/// answer against only exists in the answer. The answer must pass the BIP375 checks
/// (scripts against shares, DLEQ proofs, a share per scan key, tx frozen) and be the
/// original transaction, only the SP output scripts being new. Then the scripts,
/// the ECDH shares and proofs, `tx_modifiable`, the signatures and MuSig2 nonces /
/// partial signatures are copied into `original`, which passes the same checks
/// afterwards. Returns how many inputs gained at least one of them.
pub fn merge_sp_signed(original: &mut PsbtV2, signed: &PsbtV2) -> Result<usize, Error> {
    bip375::verify_signed(signed).map_err(|e| Error::SpSignedInvalid(e.to_string()))?;
    check_same_transaction(original, signed)?;

    for (output, signed_output) in original.outputs.iter_mut().zip(&signed.outputs) {
        if output.script_pubkey.is_none() {
            output
                .script_pubkey
                .clone_from(&signed_output.script_pubkey);
        }
    }
    copy_unknown(
        &mut original.unknown,
        &signed.unknown,
        &[PSBT_GLOBAL_SP_ECDH_SHARE, PSBT_GLOBAL_SP_DLEQ],
    );
    original.tx_modifiable = signed.tx_modifiable;

    let mut touched = 0;
    for (input, signed_input) in original.inputs.iter_mut().zip(&signed.inputs) {
        copy_unknown(
            &mut input.psbt.unknown,
            &signed_input.psbt.unknown,
            &[PSBT_IN_SP_ECDH_SHARE, PSBT_IN_SP_DLEQ],
        );
        if copy_signatures(&mut input.psbt, &signed_input.psbt) {
            touched += 1;
        }
    }

    bip375::verify_signed(original).map_err(|e| Error::SpSignedInvalid(e.to_string()))?;
    Ok(touched)
}

/// The fields that make the transaction, compared one by one since `original` has
/// no txid yet: only an SP output's script may be absent from `original`.
fn check_same_transaction(original: &PsbtV2, signed: &PsbtV2) -> Result<(), Error> {
    if original.tx_version != signed.tx_version
        || original.fallback_lock_time != signed.fallback_lock_time
    {
        return Err(Error::SignedPsbtDiffers("version or locktime"));
    }
    if original.inputs.len() != signed.inputs.len() {
        return Err(Error::SignedPsbtDiffers("inputs"));
    }
    for (input, signed_input) in original.inputs.iter().zip(&signed.inputs) {
        if input.previous_output != signed_input.previous_output
            || input.sequence != signed_input.sequence
            || input.required_time_lock_time != signed_input.required_time_lock_time
            || input.required_height_lock_time != signed_input.required_height_lock_time
        {
            return Err(Error::SignedPsbtDiffers("inputs"));
        }
    }
    if original.outputs.len() != signed.outputs.len() {
        return Err(Error::SignedPsbtDiffers("outputs"));
    }
    for (output, signed_output) in original.outputs.iter().zip(&signed.outputs) {
        if output.amount != signed_output.amount {
            return Err(Error::SignedPsbtDiffers("output amounts"));
        }
        let sp_info = |out: &bwk_sp::bwk_tx::bwk_psbt::Output| {
            unknown_of(
                &out.psbt.unknown,
                &[PSBT_OUT_SP_V0_INFO, PSBT_OUT_SP_V0_LABEL],
            )
        };
        if sp_info(output) != sp_info(signed_output) {
            return Err(Error::SignedPsbtDiffers("silent payment outputs"));
        }
        match &output.script_pubkey {
            Some(script) if signed_output.script_pubkey.as_ref() != Some(script) => {
                return Err(Error::SignedPsbtDiffers("output scripts"));
            }
            None if sp_info(output).is_empty() => {
                return Err(Error::SignedPsbtDiffers("output scripts"));
            }
            _ => {}
        }
    }
    Ok(())
}

type UnknownMap = std::collections::BTreeMap<bitcoin::psbt::raw::Key, Vec<u8>>;

/// The entries of `map` with one of `types`, in key order.
fn unknown_of(map: &UnknownMap, types: &[u8]) -> Vec<(bitcoin::psbt::raw::Key, Vec<u8>)> {
    map.iter()
        .filter(|(key, _)| types.contains(&key.type_value))
        .map(|(key, value)| (key.clone(), value.clone()))
        .collect()
}

/// Adds `from`'s entries with one of `types` to `into`, keeping `into`'s own.
fn copy_unknown(into: &mut UnknownMap, from: &UnknownMap, types: &[u8]) {
    for (key, value) in unknown_of(from, types) {
        into.entry(key).or_insert(value);
    }
}

/// Adds, to each taproot input of `participants`' wallet that lists no MuSig2
/// participants, `aggregate`'s: the internal key's of a wallet whose internal key is a
/// musig(). A Coldcard opens its MuSig2 session only for a PSBT carrying MuSig2 fields,
/// then asserts on that session for every input of a wallet with a musig() internal
/// key, even one it signs on a single-key leaf (firmware `psbt.py`,
/// `musig_process_input`: "Signing failed late"). Returns how many inputs gained it.
pub fn add_internal_participants(
    psbt: &mut PsbtV2,
    aggregate: PublicKey,
    participants: &[PublicKey],
) -> Result<usize, Error> {
    let mut added = 0;
    for input in &mut psbt.inputs {
        let input = &mut input.psbt;
        let ours = participants.iter().any(|key| {
            input
                .tap_key_origins
                .contains_key(&key.x_only_public_key().0)
        });
        if input.tap_internal_key.is_none() || !ours {
            continue;
        }
        if !musig_input_participants_all(input)
            .map_err(|e| Error::PsbtParse(e.to_string()))?
            .is_empty()
        {
            continue;
        }
        set_musig_input_participants(input, aggregate, participants);
        added += 1;
    }
    Ok(added)
}

/// Adds `signed`'s signatures to `input`, keeping the ones it has. Whether any
/// signature was added. Also copies 0x1b / 0x1c when `input` lists MuSig2
/// participants: a device may answer a nonce for a key path it was only told of to
/// sign a single-key leaf (see `add_internal_participants`). A nonce-only answer
/// counts as progress.
fn copy_signatures(input: &mut bitcoin::psbt::Input, signed: &bitcoin::psbt::Input) -> bool {
    let count = |input: &bitcoin::psbt::Input| {
        input.partial_sigs.len()
            + usize::from(input.tap_key_sig.is_some())
            + input.tap_script_sigs.len()
            + unknown_of(&input.unknown, &MUSIG_INPUT_TYPES).len()
    };
    let before = count(input);
    input.partial_sigs.extend(signed.partial_sigs.clone());
    if input.tap_key_sig.is_none() {
        input.tap_key_sig = signed.tap_key_sig;
    }
    input.tap_script_sigs.extend(signed.tap_script_sigs.clone());
    // An input listing no MuSig2 participants signs no MuSig2 round: a nonce in the
    // answer is one the device made for a key path it was only told of.
    let musig = musig_input_participants_all(input).is_ok_and(|all| !all.is_empty());
    if musig {
        copy_unknown(&mut input.unknown, &signed.unknown, &MUSIG_INPUT_TYPES);
    }
    count(input) > before
}

/// `merge_signatures` over serialized bytes: `signed` is accepted as either a v0 or
/// a v2 serialized PSBT, and the result is `original_v2`'s v2 serialization with the
/// signatures and MuSig2 nonces / partial signatures merged in. While `original_v2`
/// still has silent payment outputs to derive, `signed` is the sp() signer's answer
/// and must be v2, merged by `merge_sp_signed`. `Error::NothingSigned` when no input
/// gained any of them.
pub fn merge_signatures_bytes(original_v2: &[u8], signed: &[u8]) -> Result<Vec<u8>, Error> {
    let mut original = parse_v2(original_v2)?;
    let touched = if scan(&original)?.sp_outputs_pending {
        // A v0 PSBT cannot carry the scripts, shares and proofs the signer derived.
        let signed_v2 = PsbtV2::deserialize(signed).map_err(|_| Error::InvalidSignedPsbt)?;
        merge_sp_signed(&mut original, &signed_v2)?
    } else {
        let signed_psbt = match Psbt::deserialize(signed) {
            Ok(psbt) => psbt,
            Err(_) => PsbtV2::deserialize(signed)
                .map_err(|_| Error::InvalidSignedPsbt)?
                .into_bitcoin_psbt()
                .map_err(|_| Error::InvalidSignedPsbt)?,
        };
        merge_signatures(&mut original, &signed_psbt)?
    };
    if touched == 0 {
        return Err(Error::NothingSigned);
    }
    original
        .serialize()
        .map_err(|e| Error::PsbtSerialize(e.to_string()))
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use bitcoin::{
        absolute::LockTime,
        bip32::{ChildNumber, DerivationPath, Fingerprint, Xpriv},
        hashes::Hash,
        key::TweakedPublicKey,
        secp256k1::{All, PublicKey, Scalar, Secp256k1, SecretKey},
        taproot::{LeafVersion, TapLeafHash, TaprootBuilder},
        transaction, Amount, Network, OutPoint, Sequence, TxOut, Txid,
    };
    use bwk_sp::bwk_tx::bwk_psbt::{
        sp::{set_sp_input_spend_bip32_derivation, set_sp_input_tweak, set_sp_v0_output},
        Input as PsbtInput, Output as PsbtOutput, PsbtV2,
    };

    use bitcoin::hex::FromHex;

    use super::*;

    fn empty_psbt() -> PsbtV2 {
        PsbtV2 {
            tx_version: transaction::Version::TWO,
            fallback_lock_time: Some(LockTime::ZERO),
            tx_modifiable: None,
            xpub: Default::default(),
            proprietary: Default::default(),
            unknown: Default::default(),
            inputs: Vec::new(),
            outputs: Vec::new(),
        }
    }

    fn blank_input() -> PsbtInput {
        PsbtInput {
            previous_output: OutPoint::new(Txid::from_byte_array([0x11; 32]), 0),
            sequence: Sequence::MAX,
            required_time_lock_time: None,
            required_height_lock_time: None,
            psbt: bitcoin::psbt::Input::default(),
        }
    }

    fn blank_output(script_pubkey: Option<bitcoin::ScriptBuf>) -> PsbtOutput {
        PsbtOutput {
            amount: Amount::from_sat(1_000),
            script_pubkey,
            psbt: bitcoin::psbt::Output::default(),
        }
    }

    fn secp() -> Secp256k1<All> {
        Secp256k1::new()
    }

    fn fingerprint(byte: u8) -> Fingerprint {
        Fingerprint::from([byte; FINGERPRINT_LEN])
    }

    fn public_key(secp: &Secp256k1<All>, byte: u8) -> PublicKey {
        PublicKey::from_secret_key(
            secp,
            &SecretKey::from_slice(&[byte; 32]).expect("valid key"),
        )
    }

    fn key_source(fp: Fingerprint) -> (Fingerprint, DerivationPath) {
        (
            fp,
            DerivationPath::from(vec![ChildNumber::from_hardened_idx(0).expect("valid index")]),
        )
    }

    /// A P2TR `TxOut` whose output key is `spend_pubkey` tweaked by `tweak`, matching
    /// what `tweak_matches` expects to find as an input's `witness_utxo`.
    fn tweaked_witness_utxo(
        secp: &Secp256k1<All>,
        spend_pubkey: PublicKey,
        tweak: [u8; 32],
    ) -> TxOut {
        let scalar = Scalar::from_be_bytes(tweak).expect("valid scalar");
        let tweaked = spend_pubkey
            .add_exp_tweak(secp, &scalar)
            .expect("tweak applies");
        let (tweaked_x, _) = tweaked.x_only_public_key();
        TxOut {
            value: Amount::from_sat(1_000),
            script_pubkey: bitcoin::ScriptBuf::new_p2tr_tweaked(
                TweakedPublicKey::dangerous_assume_tweaked(tweaked_x),
            ),
        }
    }

    fn sp_descriptor_with_origin() -> (SpDescriptor, Fingerprint) {
        let secp = secp();
        let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x11; 64]).expect("valid seed");
        let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x22; 64]).expect("valid seed");
        let fp = scan_xprv.fingerprint(&secp);
        let s = format!("sp([{fp}/352h/0h/0h]{scan_xprv}/0h,{spend_xprv}/0h)");
        (SpDescriptor::from_str(&s).expect("valid sp descriptor"), fp)
    }

    fn sp_descriptor_without_origin() -> String {
        let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x11; 64]).expect("valid seed");
        let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x22; 64]).expect("valid seed");
        format!("sp({scan_xprv}/0h,{spend_xprv}/0h)")
    }

    /// A second, distinct descriptor from `sp_descriptor_with_origin`, so tests can
    /// register more than one sp() descriptor and check the resolver picks the right
    /// one.
    fn other_sp_descriptor_with_origin() -> (SpDescriptor, Fingerprint) {
        let secp = secp();
        let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x33; 64]).expect("valid seed");
        let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x44; 64]).expect("valid seed");
        let fp = scan_xprv.fingerprint(&secp);
        let s = format!("sp([{fp}/352h/0h/0h]{scan_xprv}/0h,{spend_xprv}/0h)");
        (SpDescriptor::from_str(&s).expect("valid sp descriptor"), fp)
    }

    #[test]
    fn bip32_derivations_are_required() {
        let secp = secp();
        let mut input = blank_input();
        let fp1 = fingerprint(0x01);
        let fp2 = fingerprint(0x02);
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xa1), key_source(fp1));
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xa2), key_source(fp2));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, &[], &secp).expect("inspect succeeds");
        assert!(
            artifacts.required.contains(&fp1),
            "expected {fp1} in {:?}",
            artifacts.required
        );
        assert!(
            artifacts.required.contains(&fp2),
            "expected {fp2} in {:?}",
            artifacts.required
        );
        assert!(!artifacts.sp_inputs, "no sp record was inserted");
        assert!(!artifacts.sp_outputs, "no sp record was inserted");
    }

    #[test]
    fn taproot_origins_are_required() {
        let secp = secp();
        let mut input = blank_input();
        let fp = fingerprint(0x03);
        let xonly = public_key(&secp, 0xb1).x_only_public_key().0;
        input
            .psbt
            .tap_key_origins
            .insert(xonly, (Vec::new(), key_source(fp)));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, &[], &secp).expect("inspect succeeds");
        assert!(
            artifacts.required.contains(&fp),
            "expected {fp} in {:?}",
            artifacts.required
        );
    }

    /// An input of a two-leaf tree listing `listed` of its leaves, key `inside` on
    /// leaf 0 and key `outside` with no leaf hash.
    fn two_leaf_input(listed: &[usize], inside: Fingerprint, outside: Fingerprint) -> PsbtInput {
        let secp = secp();
        let internal = public_key(&secp, 0xc1).x_only_public_key().0;
        let scripts: Vec<bitcoin::ScriptBuf> = (0..2u8)
            .map(|i| bitcoin::ScriptBuf::from_bytes(vec![0x51, i]))
            .collect();
        let mut builder = TaprootBuilder::new();
        for script in &scripts {
            builder = builder.add_leaf(1, script.clone()).expect("valid leaf");
        }
        let info = builder.finalize(&secp, internal).expect("valid tree");
        let mut input = blank_input();
        input.psbt.tap_internal_key = Some(internal);
        for index in listed {
            let script = &scripts[*index];
            let control_block = info
                .control_block(&(script.clone(), LeafVersion::TapScript))
                .expect("leaf in the tree");
            input
                .psbt
                .tap_scripts
                .insert(control_block, (script.clone(), LeafVersion::TapScript));
        }
        let leaf = TapLeafHash::from_script(&scripts[0], LeafVersion::TapScript);
        input.psbt.tap_key_origins.insert(
            public_key(&secp, 0xc2).x_only_public_key().0,
            (vec![leaf], key_source(inside)),
        );
        input.psbt.tap_key_origins.insert(
            public_key(&secp, 0xc3).x_only_public_key().0,
            (Vec::new(), key_source(outside)),
        );
        input
    }

    #[test]
    fn a_leaf_restriction_requires_only_the_leaf_keys() {
        let secp = secp();
        let inside = fingerprint(0x21);
        let outside = fingerprint(0x22);
        let mut psbt = empty_psbt();
        psbt.inputs.push(two_leaf_input(&[0], inside, outside));

        let artifacts = inspect(&psbt, &[], &secp).expect("inspect succeeds");
        assert_eq!(artifacts.required, BTreeSet::from([inside]));
    }

    #[test]
    fn a_whole_tree_requires_every_origin() {
        let secp = secp();
        let inside = fingerprint(0x23);
        let outside = fingerprint(0x24);
        let mut psbt = empty_psbt();
        psbt.inputs.push(two_leaf_input(&[0, 1], inside, outside));

        let artifacts = inspect(&psbt, &[], &secp).expect("inspect succeeds");
        assert_eq!(artifacts.required, BTreeSet::from([inside, outside]));
    }

    #[test]
    fn sp_spend_derivations_are_required() {
        let secp = secp();
        let (descriptor, fp) = sp_descriptor_with_origin();
        let spend_pubkey = descriptor.spend_public_key(&secp).expect("spend pubkey");
        let mut input = blank_input();
        set_sp_input_spend_bip32_derivation(&mut input.psbt, spend_pubkey, key_source(fp));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, &[descriptor], &secp).expect("inspect succeeds");
        assert!(
            artifacts.required.contains(&fp),
            "expected {fp} in {:?}",
            artifacts.required
        );
        assert!(artifacts.sp_inputs, "sp spend derivation marks an sp input");
        assert_eq!(
            artifacts.sp_fingerprint,
            Some(fp),
            "expected the spend derivation's fingerprint to resolve"
        );
    }

    #[test]
    fn two_sp_inputs_naming_different_fingerprints_disagree() {
        let secp = secp();
        let fp1 = fingerprint(0x0c);
        let fp2 = fingerprint(0x0d);
        let mut input1 = blank_input();
        set_sp_input_spend_bip32_derivation(
            &mut input1.psbt,
            public_key(&secp, 0xe1),
            key_source(fp1),
        );
        let mut input2 = blank_input();
        set_sp_input_spend_bip32_derivation(
            &mut input2.psbt,
            public_key(&secp, 0xe2),
            key_source(fp2),
        );
        let mut psbt = empty_psbt();
        psbt.inputs.push(input1);
        psbt.inputs.push(input2);

        let result = inspect(&psbt, &[], &secp);
        assert!(
            matches!(result, Err(Error::SpInputsDisagree)),
            "expected SpInputsDisagree for two sp inputs naming different fingerprints, got {result:?}"
        );
    }

    #[test]
    fn a_short_sp_spend_derivation_value_errors() {
        let secp = secp();
        let mut input = blank_input();
        input.psbt.unknown.insert(
            bitcoin::psbt::raw::Key {
                type_value: PSBT_IN_SP_SPEND_BIP32_DERIVATION,
                key: vec![0xaa; 33],
            },
            vec![0x01, 0x02, 0x03],
        );
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let result = inspect(&psbt, &[], &secp);
        assert!(
            matches!(result, Err(Error::SpField(_))),
            "expected SpField error, got {result:?}"
        );
    }

    #[test]
    fn a_fingerprint_reached_twice_appears_once() {
        let secp = secp();
        let mut input = blank_input();
        let fp = fingerprint(0x05);
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xd1), key_source(fp));
        let xonly = public_key(&secp, 0xd2).x_only_public_key().0;
        input
            .psbt
            .tap_key_origins
            .insert(xonly, (Vec::new(), key_source(fp)));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, &[], &secp).expect("inspect succeeds");
        assert_eq!(
            artifacts.required.len(),
            1,
            "expected a single fingerprint, got {:?}",
            artifacts.required
        );
    }

    #[test]
    fn an_sp_tweak_marks_an_sp_input() {
        let secp = secp();
        let (descriptor, fp) = sp_descriptor_with_origin();
        let spend_pubkey = descriptor.spend_public_key(&secp).expect("spend pubkey");
        let tweak = [0x07u8; 32];
        let mut input = blank_input();
        input.psbt.witness_utxo = Some(tweaked_witness_utxo(&secp, spend_pubkey, tweak));
        set_sp_input_tweak(&mut input.psbt, tweak);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, &[descriptor], &secp).expect("inspect succeeds");
        assert!(artifacts.sp_inputs, "sp tweak marks an sp input");
        assert_eq!(
            artifacts.sp_fingerprint,
            Some(fp),
            "expected the tweak to resolve to the descriptor's fingerprint"
        );
    }

    #[test]
    fn tweak_only_resolution_picks_the_one_descriptor_whose_spend_key_matches() {
        let secp = secp();
        let (descriptor_a, _fp_a) = sp_descriptor_with_origin();
        let (descriptor_b, fp_b) = other_sp_descriptor_with_origin();
        let spend_pubkey_b = descriptor_b.spend_public_key(&secp).expect("spend pubkey");
        let tweak = [0x0eu8; 32];
        let mut input = blank_input();
        input.psbt.witness_utxo = Some(tweaked_witness_utxo(&secp, spend_pubkey_b, tweak));
        set_sp_input_tweak(&mut input.psbt, tweak);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts =
            inspect(&psbt, &[descriptor_a, descriptor_b], &secp).expect("inspect succeeds");
        assert_eq!(
            artifacts.sp_fingerprint,
            Some(fp_b),
            "expected the tweak to resolve to descriptor b's fingerprint out of two \
             registered descriptors, got {:?}",
            artifacts.sp_fingerprint
        );
    }

    #[test]
    fn an_sp_tweak_matching_no_registered_descriptor_errors() {
        let secp = secp();
        let mut input = blank_input();
        set_sp_input_tweak(&mut input.psbt, [0x08; 32]);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let result = inspect(&psbt, &[], &secp);
        assert!(
            matches!(result, Err(Error::SpKeyUnknown(None))),
            "expected SpKeyUnknown(None), got {result:?}"
        );
    }

    #[test]
    fn an_sp_descriptor_without_an_origin_cannot_resolve_even_on_a_matching_tweak() {
        let secp = secp();
        let s = sp_descriptor_without_origin();
        let descriptor = SpDescriptor::from_str(&s).expect("valid sp descriptor");
        let spend_pubkey = descriptor.spend_public_key(&secp).expect("spend pubkey");
        let tweak = [0x09u8; 32];
        let mut input = blank_input();
        input.psbt.witness_utxo = Some(tweaked_witness_utxo(&secp, spend_pubkey, tweak));
        set_sp_input_tweak(&mut input.psbt, tweak);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let result = inspect(&psbt, &[descriptor], &secp);
        assert!(
            matches!(result, Err(Error::SpKeyUnknown(None))),
            "expected SpKeyUnknown(None) even though the tweak matches, since the descriptor \
             has no origin, got {result:?}"
        );
    }

    #[test]
    fn an_sp_descriptor_with_an_origin_contributes_its_fingerprint() {
        let secp = secp();
        let (descriptor, fp) = sp_descriptor_with_origin();
        let spend_pubkey = descriptor.spend_public_key(&secp).expect("spend pubkey");
        let tweak = [0x0au8; 32];
        let mut input = blank_input();
        input.psbt.witness_utxo = Some(tweaked_witness_utxo(&secp, spend_pubkey, tweak));
        set_sp_input_tweak(&mut input.psbt, tweak);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, &[descriptor], &secp).expect("inspect succeeds");
        assert!(
            artifacts.required.contains(&fp),
            "expected {fp} in {:?}",
            artifacts.required
        );
        assert_eq!(
            artifacts.sp_fingerprint,
            Some(fp),
            "expected sp_fingerprint to be {fp}"
        );
    }

    #[test]
    fn a_pending_output_with_no_sp_input_data_is_unresolvable() {
        let secp = secp();
        let mut psbt = empty_psbt();
        psbt.outputs.push(blank_output(None));

        let result = inspect(&psbt, &[], &secp);
        assert!(
            matches!(result, Err(Error::SpOutputsUnresolvable)),
            "expected SpOutputsUnresolvable, got {result:?}"
        );
    }

    #[test]
    fn a_pending_output_with_one_input_owner_resolves_to_that_key() {
        let secp = secp();
        let (descriptor, fp) = sp_descriptor_with_origin();
        let mut input = blank_input();
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xa1), key_source(fp));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);
        psbt.outputs.push(blank_output(None));

        let artifacts = inspect(&psbt, &[descriptor], &secp).expect("inspect succeeds");
        assert_eq!(artifacts.sp_fingerprint, Some(fp));
        // Without that key's sp() descriptor it cannot derive the outputs.
        assert!(matches!(
            inspect(&psbt, &[], &secp),
            Err(Error::SpKeyUnknown(Some(unknown))) if unknown == fp
        ));
    }

    #[test]
    fn a_pending_output_with_two_input_owners_is_unresolvable() {
        let secp = secp();
        let (descriptor, fp) = sp_descriptor_with_origin();
        let mut input = blank_input();
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xa1), key_source(fp));
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xa2), key_source(fingerprint(0x02)));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);
        psbt.outputs.push(blank_output(None));

        assert!(matches!(
            inspect(&psbt, &[descriptor], &secp),
            Err(Error::SpOutputsUnresolvable)
        ));
    }

    #[test]
    fn a_filled_output_has_no_pending_flag() {
        let secp = secp();
        let mut psbt = empty_psbt();
        psbt.outputs
            .push(blank_output(Some(bitcoin::ScriptBuf::new())));

        let artifacts = inspect(&psbt, &[], &secp).expect("inspect succeeds");
        assert!(
            !artifacts.sp_outputs_pending,
            "expected no pending output once script is filled"
        );
    }

    #[test]
    fn a_non_sp_psbt_ignores_the_descriptor() {
        let secp = secp();
        let (descriptor, _) = sp_descriptor_with_origin();
        let mut input = blank_input();
        let fp = fingerprint(0x0b);
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xe1), key_source(fp));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, &[descriptor], &secp).expect("inspect succeeds");
        assert_eq!(
            artifacts.sp_fingerprint, None,
            "non sp psbt must not set sp_fingerprint"
        );
        assert_eq!(
            artifacts.required.len(),
            1,
            "descriptor fingerprint must not be added, got {:?}",
            artifacts.required
        );
    }

    #[test]
    fn sp_outputs_are_detected() {
        let secp = secp();
        let mut output = blank_output(Some(bitcoin::ScriptBuf::new()));
        set_sp_v0_output(
            &mut output.psbt,
            public_key(&secp, 0xf1),
            public_key(&secp, 0xf2),
            None,
        );
        let mut psbt = empty_psbt();
        psbt.outputs.push(output);

        let artifacts = inspect(&psbt, &[], &secp).expect("inspect succeeds");
        assert!(
            artifacts.sp_outputs,
            "expected the sp output to be detected"
        );
        assert!(
            !artifacts.sp_outputs_pending,
            "expected no pending flag for a filled output"
        );
        assert_eq!(
            artifacts.sp_fingerprint, None,
            "expected no sp fingerprint to resolve without any sp input data"
        );
    }

    /// Kern's answer to the bwk-sp fixture PSBT (pythcoiner/silent#39): SP output
    /// scripts derived, global ECDH shares and DLEQ proofs, the input key-path
    /// signed, tx frozen.
    const KERN_SP_SIGNED: &str = "70736274ff0102040200000001030400000000010401010105010201fb040200000022070220bcfac5b99e04ad1a06ddfb016ee13582609d60b6291e98d01a9bc9a16c96d42103828fd168088f316588c4f7523fa8e438160d9db9273b27d720762b02b4ce0b7322080220bcfac5b99e04ad1a06ddfb016ee13582609d60b6291e98d01a9bc9a16c96d440310e5048750b1c7a36689c1de6dcdd815112dd485821bb58a0fdc15c4c5483789638284811e9b00e3af6aadf308ee9e1aa7adeeddb0e53128879c1cd6c4e31942207024139b0f81042e243a90478e43990c6a27be0e3346f0c71adbbcdd511beaea1e321030899d7c63650b59990c011ca08c1893f75b62aa9a2038f0125506fbb633a93c92208024139b0f81042e243a90478e43990c6a27be0e3346f0c71adbbcdd511beaea1e3400c2f27b26d92589af6819cdb225c231780c1375658f7d8854ef413f99d398d3603e0f74711ae86208ed249f4274283de159aae9da781a22b888040dcf6e7cd1c010601000001012ba086010000000000225120cba15611f961103c8738f27fefa7b774bc6b242f17b55dae832a3ae2efe332ed010e202222222222222222222222222222222222222222222222222222222222222222010f0400000000011340e3a02ce19eb5a94a10331cede6754f1456351b85b41f0d87b2214000a319d7069e027cb9c3e744e1b232bdd3d800ffa0f67be63bbbe32baf987b60b9c6b9c9650001030860ea0000000000000104225120a5675fd49d557606b5b541b345b3b0f83733e6bf21d3442884f9949872b658820109420220bcfac5b99e04ad1a06ddfb016ee13582609d60b6291e98d01a9bc9a16c96d4025cc9856d6f8375350e123978daac200c260cb5b5ae83106cab90484dcd8fcf3600010308589800000000000001042251206608dcdf249f25bbb94eda8f31ab3c05f6fd3e762a3c9e41152342eb4b38a546010942024139b0f81042e243a90478e43990c6a27be0e3346f0c71adbbcdd511beaea1e3031ec096b3e9917a43e81c6d014acab0dfa32151ea7dcdfa56a1dc89f0aec04ef8010a040000000000";

    fn kern_signed() -> PsbtV2 {
        let bytes = Vec::<u8>::from_hex(KERN_SP_SIGNED).unwrap();
        PsbtV2::deserialize(&bytes).unwrap()
    }

    /// The PSBT Silent sent for that answer: the same transaction with its SP
    /// outputs underived, no share, proof or signature, and the tx still modifiable.
    fn kern_original() -> PsbtV2 {
        let mut psbt = kern_signed();
        for output in &mut psbt.outputs {
            if sp_v0_output(&output.psbt).unwrap().is_some() {
                output.script_pubkey = None;
            }
        }
        psbt.unknown.retain(|key, _| {
            key.type_value != PSBT_GLOBAL_SP_ECDH_SHARE && key.type_value != PSBT_GLOBAL_SP_DLEQ
        });
        for input in &mut psbt.inputs {
            input.psbt.tap_key_sig = None;
            input.psbt.partial_sigs.clear();
            input.psbt.tap_script_sigs.clear();
            input.psbt.unknown.retain(|key, _| {
                key.type_value != PSBT_IN_SP_ECDH_SHARE && key.type_value != PSBT_IN_SP_DLEQ
            });
        }
        psbt.tx_modifiable = None;
        psbt
    }

    #[test]
    fn merges_the_sp_signers_answer_into_the_scriptless_original() {
        let original = kern_original();
        assert!(scan(&original).unwrap().sp_outputs_pending);
        let original_bytes = original.serialize().unwrap();
        let signed_bytes = Vec::<u8>::from_hex(KERN_SP_SIGNED).unwrap();

        let merged =
            parse_v2(&merge_signatures_bytes(&original_bytes, &signed_bytes).unwrap()).unwrap();

        let signed = kern_signed();
        for (output, signed_output) in merged.outputs.iter().zip(&signed.outputs) {
            assert_eq!(output.script_pubkey, signed_output.script_pubkey);
            assert!(output.script_pubkey.is_some());
        }
        assert_eq!(
            unknown_of(
                &merged.unknown,
                &[PSBT_GLOBAL_SP_ECDH_SHARE, PSBT_GLOBAL_SP_DLEQ]
            ),
            unknown_of(
                &signed.unknown,
                &[PSBT_GLOBAL_SP_ECDH_SHARE, PSBT_GLOBAL_SP_DLEQ]
            )
        );
        assert!(!unknown_of(&merged.unknown, &[PSBT_GLOBAL_SP_ECDH_SHARE]).is_empty());
        assert!(bip375::verify_signed(&merged).is_ok());
        assert!(!scan(&merged).unwrap().sp_outputs_pending);

        // Finalizable: the key-path signature is the whole witness.
        let mut finalized = merged.into_bitcoin_psbt().unwrap();
        for input in &mut finalized.inputs {
            let sig = input.tap_key_sig.expect("every input is signed");
            input.final_script_witness = Some(bitcoin::Witness::p2tr_key_spend(&sig));
        }
        let tx = finalized.extract_tx_unchecked_fee_rate();
        assert_eq!(
            tx.compute_txid(),
            signed.unsigned_tx().unwrap().compute_txid()
        );
    }

    #[test]
    fn refuses_an_sp_answer_for_another_transaction() {
        let original_bytes = kern_original().serialize().unwrap();
        let mut original = kern_original();
        original.outputs[0].amount = bitcoin::Amount::from_sat(1);
        let other_bytes = original.serialize().unwrap();
        let signed_bytes = Vec::<u8>::from_hex(KERN_SP_SIGNED).unwrap();
        assert!(matches!(
            merge_signatures_bytes(&other_bytes, &signed_bytes),
            Err(Error::SignedPsbtDiffers(_))
        ));
        // The genuine original still merges.
        assert!(merge_signatures_bytes(&original_bytes, &signed_bytes).is_ok());
    }

    #[test]
    fn refuses_an_sp_answer_that_leaves_outputs_scriptless() {
        let original_bytes = kern_original().serialize().unwrap();
        assert!(matches!(
            merge_signatures_bytes(&original_bytes, &original_bytes),
            Err(Error::SpSignedInvalid(_))
        ));
    }

    #[test]
    fn refuses_a_v0_answer_to_pending_sp_outputs() {
        let original_bytes = kern_original().serialize().unwrap();
        let v0 = kern_signed().into_bitcoin_psbt().unwrap().serialize();
        assert!(matches!(
            merge_signatures_bytes(&original_bytes, &v0),
            Err(Error::InvalidSignedPsbt)
        ));
    }
}
