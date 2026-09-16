use std::collections::BTreeSet;

use bitcoin::{
    bip32::Fingerprint,
    secp256k1::{All, PublicKey, Scalar, Secp256k1},
    Psbt, ScriptBuf,
};
use bwk_sp::{
    bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor,
    bwk_tx::bwk_psbt::{
        sp::{sp_input_tweak, sp_v0_output, PSBT_IN_SP_SPEND_BIP32_DERIVATION, PSBT_IN_SP_TWEAK},
        Error as PsbtV2Error, PsbtV2,
    },
};

use crate::keys::error::Error;

const FINGERPRINT_LEN: usize = 4;

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
    let mut sp_inputs = false;

    for input in &psbt.inputs {
        for (fingerprint, _) in input.psbt.bip32_derivation.values() {
            required.insert(*fingerprint);
        }
        for (_, (fingerprint, _)) in input.psbt.tap_key_origins.values() {
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
        return Err(Error::SpOutputsUnresolvable);
    }

    Ok(None)
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
    })
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
/// from `signed` into `original`, adding entries and never removing existing ones.
/// Refuses a `signed` PSBT for a different transaction, and an `original` PSBT with
/// an output the sp() key has not derived yet, since the txid to check `signed`
/// against does not exist until that output is derived. Returns how many inputs
/// gained at least one signature.
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
        let before = input.psbt.partial_sigs.len()
            + usize::from(input.psbt.tap_key_sig.is_some())
            + input.psbt.tap_script_sigs.len();

        input
            .psbt
            .partial_sigs
            .extend(signed_input.partial_sigs.clone());
        if input.psbt.tap_key_sig.is_none() {
            input.psbt.tap_key_sig = signed_input.tap_key_sig;
        }
        input
            .psbt
            .tap_script_sigs
            .extend(signed_input.tap_script_sigs.clone());

        let after = input.psbt.partial_sigs.len()
            + usize::from(input.psbt.tap_key_sig.is_some())
            + input.psbt.tap_script_sigs.len();
        if after > before {
            touched += 1;
        }
    }
    Ok(touched)
}

/// `merge_signatures` over serialized bytes: `signed` is accepted as either a v0 or
/// a v2 serialized PSBT, and the result is `original_v2`'s v2 serialization with the
/// signatures merged in. `Error::NothingSigned` when no input gained a signature.
pub fn merge_signatures_bytes(original_v2: &[u8], signed: &[u8]) -> Result<Vec<u8>, Error> {
    let mut original = parse_v2(original_v2)?;
    let signed_psbt = match Psbt::deserialize(signed) {
        Ok(psbt) => psbt,
        Err(_) => PsbtV2::deserialize(signed)
            .map_err(|_| Error::InvalidSignedPsbt)?
            .into_bitcoin_psbt()
            .map_err(|_| Error::InvalidSignedPsbt)?,
    };

    let touched = merge_signatures(&mut original, &signed_psbt)?;
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
        transaction, Amount, Network, OutPoint, Sequence, TxOut, Txid,
    };
    use bwk_sp::bwk_tx::bwk_psbt::{
        sp::{set_sp_input_spend_bip32_derivation, set_sp_input_tweak, set_sp_v0_output},
        Input as PsbtInput, Output as PsbtOutput, PsbtV2,
    };

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
}
