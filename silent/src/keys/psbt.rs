use std::collections::BTreeSet;

use bitcoin::bip32::Fingerprint;
use bwk_sp::{
    bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor,
    bwk_tx::bwk_psbt::{
        sp::{sp_v0_output, PSBT_IN_SP_SPEND_BIP32_DERIVATION, PSBT_IN_SP_TWEAK},
        PsbtV2,
    },
};

use crate::keys::error::Error;

const FINGERPRINT_LEN: usize = 4;

/// What a PSBT requires to be fully signed, worked out from the PSBT alone.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PsbtArtifacts {
    /// Every fingerprint that has to sign, in a deterministic order.
    pub required: BTreeSet<Fingerprint>,
    /// The fingerprint of the sp() descriptor, when the PSBT carries SP artifacts.
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

pub fn inspect(
    psbt: &PsbtV2,
    sp_descriptor: Option<&SpDescriptor>,
) -> Result<PsbtArtifacts, Error> {
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

    let mut sp_fingerprint = None;
    if sp_inputs || sp_outputs {
        let descriptor = sp_descriptor.ok_or(Error::MissingSpDescriptor)?;
        let fingerprint = descriptor
            .fingerprint()
            .ok_or_else(|| Error::MissingSpOrigin(descriptor.to_string()))?;
        required.insert(fingerprint);
        sp_fingerprint = Some(fingerprint);
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
    sp_descriptor: Option<&SpDescriptor>,
) -> Result<PsbtArtifacts, Error> {
    let psbt = parse_v2(bytes)?;
    inspect(&psbt, sp_descriptor)
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use bitcoin::{
        absolute::LockTime,
        bip32::{ChildNumber, DerivationPath, Fingerprint, Xpriv},
        hashes::Hash,
        secp256k1::{PublicKey, Secp256k1, SecretKey},
        transaction, Amount, Network, OutPoint, Sequence, Txid,
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

    fn secp() -> Secp256k1<bitcoin::secp256k1::All> {
        Secp256k1::new()
    }

    fn fingerprint(byte: u8) -> Fingerprint {
        Fingerprint::from([byte; FINGERPRINT_LEN])
    }

    fn public_key(secp: &Secp256k1<bitcoin::secp256k1::All>, byte: u8) -> PublicKey {
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

        let artifacts = inspect(&psbt, None).expect("inspect succeeds");
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

        let artifacts = inspect(&psbt, None).expect("inspect succeeds");
        assert!(
            artifacts.required.contains(&fp),
            "expected {fp} in {:?}",
            artifacts.required
        );
    }

    #[test]
    fn sp_spend_derivations_are_required() {
        let secp = secp();
        let (descriptor, _) = sp_descriptor_with_origin();
        let mut input = blank_input();
        let fp = fingerprint(0x04);
        let spend = public_key(&secp, 0xc1);
        set_sp_input_spend_bip32_derivation(&mut input.psbt, spend, key_source(fp));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, Some(&descriptor)).expect("inspect succeeds");
        assert!(
            artifacts.required.contains(&fp),
            "expected {fp} in {:?}",
            artifacts.required
        );
        assert!(artifacts.sp_inputs, "sp spend derivation marks an sp input");
    }

    #[test]
    fn a_short_sp_spend_derivation_value_errors() {
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

        let result = inspect(&psbt, None);
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

        let artifacts = inspect(&psbt, None).expect("inspect succeeds");
        assert_eq!(
            artifacts.required.len(),
            1,
            "expected a single fingerprint, got {:?}",
            artifacts.required
        );
    }

    #[test]
    fn an_sp_tweak_marks_an_sp_input() {
        let (descriptor, _) = sp_descriptor_with_origin();
        let mut input = blank_input();
        set_sp_input_tweak(&mut input.psbt, [0x07; 32]);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, Some(&descriptor)).expect("inspect succeeds");
        assert!(artifacts.sp_inputs, "sp tweak marks an sp input");
    }

    #[test]
    fn an_sp_psbt_without_a_descriptor_errors() {
        let mut input = blank_input();
        set_sp_input_tweak(&mut input.psbt, [0x08; 32]);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let result = inspect(&psbt, None);
        assert!(
            matches!(result, Err(Error::MissingSpDescriptor)),
            "expected MissingSpDescriptor, got {result:?}"
        );
    }

    #[test]
    fn an_sp_descriptor_without_an_origin_errors() {
        let s = sp_descriptor_without_origin();
        let descriptor = SpDescriptor::from_str(&s).expect("valid sp descriptor");
        let mut input = blank_input();
        set_sp_input_tweak(&mut input.psbt, [0x09; 32]);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let result = inspect(&psbt, Some(&descriptor));
        match result {
            Err(Error::MissingSpOrigin(found)) => {
                assert_eq!(found, descriptor.to_string(), "descriptor string mismatch");
            }
            other => panic!("expected MissingSpOrigin, got {other:?}"),
        }
    }

    #[test]
    fn an_sp_descriptor_with_an_origin_contributes_its_fingerprint() {
        let (descriptor, fp) = sp_descriptor_with_origin();
        let mut input = blank_input();
        set_sp_input_tweak(&mut input.psbt, [0x0a; 32]);
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, Some(&descriptor)).expect("inspect succeeds");
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
    fn pending_outputs_are_detected() {
        let mut psbt = empty_psbt();
        psbt.outputs.push(blank_output(None));
        let artifacts = inspect(&psbt, None).expect("inspect succeeds");
        assert!(artifacts.sp_outputs_pending, "expected pending output");

        let mut filled_psbt = empty_psbt();
        filled_psbt
            .outputs
            .push(blank_output(Some(bitcoin::ScriptBuf::new())));
        let filled_artifacts = inspect(&filled_psbt, None).expect("inspect succeeds");
        assert!(
            !filled_artifacts.sp_outputs_pending,
            "expected no pending output once script is filled"
        );
    }

    #[test]
    fn a_non_sp_psbt_ignores_the_descriptor() {
        let (descriptor, _) = sp_descriptor_with_origin();
        let secp = secp();
        let mut input = blank_input();
        let fp = fingerprint(0x0b);
        input
            .psbt
            .bip32_derivation
            .insert(public_key(&secp, 0xe1), key_source(fp));
        let mut psbt = empty_psbt();
        psbt.inputs.push(input);

        let artifacts = inspect(&psbt, Some(&descriptor)).expect("inspect succeeds");
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

        let artifacts = inspect(&psbt, None);
        assert!(
            matches!(artifacts, Err(Error::MissingSpDescriptor)),
            "sp output with no descriptor must error, got {artifacts:?}"
        );
    }
}
