//! Fingerprint discovery from a raw PSBT, and the signing plan built on top of it.
//!
//! Cases 1-4 and 6 exercise the descriptor-free walk and the origin check directly;
//! they need no key store. Cases 5 and 7 go through `KeysModule::signing_plan`,
//! which reads the process-wide datadir, so they share the datadir helper below.

use std::str::FromStr;

use bitcoin::{
    absolute::LockTime,
    bip32::{ChildNumber, DerivationPath, Fingerprint, Xpriv},
    hashes::Hash,
    secp256k1::{PublicKey, Secp256k1, SecretKey},
    transaction, Amount, Network, NetworkKind, OutPoint, Psbt, ScriptBuf, Sequence, TxOut, Txid,
};
use bwk_sp::bwk_sign::{
    bwk_descriptor::{
        descriptor::{wpkh, wpkh_path, DescriptorDerivator},
        sp_descriptor::SpDescriptor,
        sp_key::{SpKey, SpScanKey},
        wallet::to_wallet,
    },
    hot_signer::HotSigner,
};
use bwk_sp::bwk_tx::bwk_psbt::{
    sp::{
        set_sp_input_ecdh_share, set_sp_input_spend_bip32_derivation, set_sp_input_tweak,
        set_sp_v0_output, sp_input_ecdh_share, sp_input_tweak,
    },
    Input as PsbtInput, Output as PsbtOutput, PsbtV2,
};
use silent::keys::{
    psbt::{inspect_bytes, merge_signatures_bytes},
    Error,
};
use silent::{KeyRouteKind, KeyUnusableReason, Network as FfiNetwork};

// ===== Datadir =====

static DATADIR: std::sync::OnceLock<temp_dir::TempDir> = std::sync::OnceLock::new();

fn datadir() -> &'static std::path::Path {
    DATADIR
        .get_or_init(|| {
            let tmp = temp_dir::TempDir::new().expect("create temp dir");
            silent::set_datadir(tmp.path().to_path_buf());
            silent::state_crypt::unlock_for_tests();
            tmp
        })
        .path()
}

// ===== PSBT fixtures =====

const TEST_FINGERPRINT_HEX: &str = "73c5da0a";
const TEST_FINGERPRINT_BYTES: [u8; 4] = [0x73, 0xc5, 0xda, 0x0a];

fn secp() -> Secp256k1<bitcoin::secp256k1::All> {
    Secp256k1::new()
}

fn public_key(secp: &Secp256k1<bitcoin::secp256k1::All>, byte: u8) -> PublicKey {
    PublicKey::from_secret_key(
        secp,
        &SecretKey::from_slice(&[byte; 32]).expect("valid key"),
    )
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

fn blank_output(script_pubkey: Option<ScriptBuf>) -> PsbtOutput {
    PsbtOutput {
        amount: Amount::from_sat(1_000),
        script_pubkey,
        psbt: bitcoin::psbt::Output::default(),
    }
}

/// A v2 PSBT with one input and one output, so it always round trips through the
/// bitcoin v0 bridge that `deserialize`/the FFI hex path go through.
fn one_input_psbt(input: PsbtInput, output: PsbtOutput) -> PsbtV2 {
    PsbtV2 {
        tx_version: transaction::Version::TWO,
        fallback_lock_time: Some(LockTime::ZERO),
        tx_modifiable: None,
        xpub: Default::default(),
        proprietary: Default::default(),
        unknown: Default::default(),
        inputs: vec![input],
        outputs: vec![output],
    }
}

fn key_source(fp: Fingerprint) -> (Fingerprint, DerivationPath) {
    (
        fp,
        DerivationPath::from(vec![ChildNumber::from_hardened_idx(0).expect("valid index")]),
    )
}

fn bip32_psbt(fp: Fingerprint) -> PsbtV2 {
    let secp = secp();
    let mut input = blank_input();
    input
        .psbt
        .bip32_derivation
        .insert(public_key(&secp, 0xa1), key_source(fp));
    one_input_psbt(input, blank_output(Some(ScriptBuf::new())))
}

fn tap_key_origins_psbt(fp: Fingerprint) -> PsbtV2 {
    let secp = secp();
    let mut input = blank_input();
    let xonly = public_key(&secp, 0xb1).x_only_public_key().0;
    input
        .psbt
        .tap_key_origins
        .insert(xonly, (Vec::new(), key_source(fp)));
    one_input_psbt(input, blank_output(Some(ScriptBuf::new())))
}

fn sp_spend_derivation_psbt(fp: Fingerprint) -> PsbtV2 {
    let secp = secp();
    let mut input = blank_input();
    set_sp_input_spend_bip32_derivation(&mut input.psbt, public_key(&secp, 0xc1), key_source(fp));
    one_input_psbt(input, blank_output(Some(ScriptBuf::new())))
}

fn sp_pending_output_psbt() -> PsbtV2 {
    let secp = secp();
    let mut output = blank_output(None);
    set_sp_v0_output(
        &mut output.psbt,
        public_key(&secp, 0xf1),
        public_key(&secp, 0xf2),
        None,
    );
    one_input_psbt(blank_input(), output)
}

/// A watch-only sp() descriptor with no origin at all.
fn sp_descriptor_without_origin() -> String {
    let secp = secp();
    let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x51; 64]).expect("valid seed");
    let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x52; 64]).expect("valid seed");
    SpDescriptor::Packed {
        origin: None,
        key: SpKey::Scan(SpScanKey {
            scan_key: scan_xprv.private_key,
            spend_key: PublicKey::from_secret_key(&secp, &spend_xprv.private_key),
            network: NetworkKind::Main,
        }),
    }
    .to_string()
}

/// A watch-only sp() descriptor (private scan key, public spend key only), with an
/// origin, so it both resolves a fingerprint and passes registration's watch-only
/// check.
fn sp_descriptor_with_origin() -> (SpDescriptor, Fingerprint) {
    let secp = secp();
    let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x61; 64]).expect("valid seed");
    let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x62; 64]).expect("valid seed");
    let fp = scan_xprv.fingerprint(&secp);
    let origin = (
        fp,
        DerivationPath::from_str("352h/0h/0h").expect("valid path"),
    );
    let descriptor = SpDescriptor::Packed {
        origin: Some(origin),
        key: SpKey::Scan(SpScanKey {
            scan_key: scan_xprv.private_key,
            spend_key: PublicKey::from_secret_key(&secp, &spend_xprv.private_key),
            network: NetworkKind::Main,
        }),
    };
    (descriptor, fp)
}

/// A second, distinct watch-only sp() descriptor with an origin, so tests can
/// register more than one sp() key and check the resolver picks the right one.
fn second_sp_descriptor_with_origin() -> (SpDescriptor, Fingerprint) {
    let secp = secp();
    let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x71; 64]).expect("valid seed");
    let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x72; 64]).expect("valid seed");
    let fp = scan_xprv.fingerprint(&secp);
    let origin = (
        fp,
        DerivationPath::from_str("352h/0h/0h").expect("valid path"),
    );
    let descriptor = SpDescriptor::Packed {
        origin: Some(origin),
        key: SpKey::Scan(SpScanKey {
            scan_key: scan_xprv.private_key,
            spend_key: PublicKey::from_secret_key(&secp, &spend_xprv.private_key),
            network: NetworkKind::Main,
        }),
    };
    (descriptor, fp)
}

/// A third, distinct watch-only sp() descriptor with an origin, for tests that need
/// two descriptors of their own without colliding with fingerprints other tests
/// register on the shared test datadir.
fn third_sp_descriptor_with_origin() -> (SpDescriptor, Fingerprint) {
    let secp = secp();
    let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x81; 64]).expect("valid seed");
    let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x82; 64]).expect("valid seed");
    let fp = scan_xprv.fingerprint(&secp);
    let origin = (
        fp,
        DerivationPath::from_str("352h/0h/0h").expect("valid path"),
    );
    let descriptor = SpDescriptor::Packed {
        origin: Some(origin),
        key: SpKey::Scan(SpScanKey {
            scan_key: scan_xprv.private_key,
            spend_key: PublicKey::from_secret_key(&secp, &spend_xprv.private_key),
            network: NetworkKind::Main,
        }),
    };
    (descriptor, fp)
}

// ===== Cases 1-3: fingerprints straight off the PSBT =====

#[test]
fn test_fingerprint_from_bip32_derivation() {
    let fp = Fingerprint::from(TEST_FINGERPRINT_BYTES);
    let bytes = bip32_psbt(fp).serialize().expect("serialize psbt");
    let result = silent::psbt_required_fingerprints(&hex::encode(bytes));

    assert!(result.is_ok, "expected ok, got error: {}", result.error);
    assert_eq!(
        result.fingerprints,
        vec![TEST_FINGERPRINT_HEX.to_string()],
        "expected exactly the bip32_derivation fingerprint, got {:?}",
        result.fingerprints
    );
    assert_eq!(
        result.fingerprints.len(),
        1,
        "expected a single fingerprint, got {}",
        result.fingerprints.len()
    );
}

#[test]
fn test_fingerprint_from_tap_key_origins() {
    let fp = Fingerprint::from(TEST_FINGERPRINT_BYTES);
    let bytes = tap_key_origins_psbt(fp)
        .serialize()
        .expect("serialize psbt");
    let result = silent::psbt_required_fingerprints(&hex::encode(bytes));

    assert!(result.is_ok, "expected ok, got error: {}", result.error);
    assert_eq!(
        result.fingerprints,
        vec![TEST_FINGERPRINT_HEX.to_string()],
        "expected exactly the tap_key_origins fingerprint, got {:?}",
        result.fingerprints
    );
}

#[test]
fn test_fingerprint_from_sp_spend_derivation() {
    let fp = Fingerprint::from(TEST_FINGERPRINT_BYTES);
    let bytes = sp_spend_derivation_psbt(fp)
        .serialize()
        .expect("serialize psbt");
    let result = silent::psbt_required_fingerprints(&hex::encode(bytes));

    assert!(result.is_ok, "expected ok, got error: {}", result.error);
    assert_eq!(
        result.fingerprints,
        vec![TEST_FINGERPRINT_HEX.to_string()],
        "expected the sp spend derivation fingerprint, a generic walk would miss it, got {:?}",
        result.fingerprints
    );
}

// ===== Case 4: a pending sp output with no sp input data is refused =====

#[test]
fn test_sp_pending_output_with_no_sp_input_data_is_unresolvable() {
    let (descriptor, _) = sp_descriptor_with_origin();
    let bytes = sp_pending_output_psbt()
        .serialize()
        .expect("serialize psbt");

    let result = inspect_bytes(&bytes, &[descriptor], &secp());
    assert!(
        matches!(result, Err(Error::SpOutputsUnresolvable)),
        "expected SpOutputsUnresolvable even with a descriptor registered, got {result:?}"
    );
}

// ===== Case 5: the sp() key signs first and blocks every other key =====

#[test]
fn test_plan_marks_sp_outputs_pending_and_orders_sp_first() {
    let _ = datadir();
    let secp = secp();
    let (descriptor, sp_fp) = sp_descriptor_with_origin();
    let spend_pubkey = descriptor.spend_public_key(&secp).expect("spend pubkey");
    let other_fp = Fingerprint::from([0x44u8, 0x44, 0x44, 0x44]);

    let mut sp_input = blank_input();
    set_sp_input_spend_bip32_derivation(&mut sp_input.psbt, spend_pubkey, key_source(sp_fp));

    let mut other_input = blank_input();
    other_input.previous_output = OutPoint::new(Txid::from_byte_array([0x22; 32]), 0);
    other_input
        .psbt
        .bip32_derivation
        .insert(public_key(&secp, 0xb2), key_source(other_fp));

    let mut output = blank_output(None);
    set_sp_v0_output(
        &mut output.psbt,
        public_key(&secp, 0xc3),
        public_key(&secp, 0xc4),
        None,
    );
    let bytes = PsbtV2 {
        tx_version: transaction::Version::TWO,
        fallback_lock_time: Some(LockTime::ZERO),
        tx_modifiable: None,
        xpub: Default::default(),
        proprietary: Default::default(),
        unknown: Default::default(),
        inputs: vec![sp_input, other_input],
        outputs: vec![output],
    }
    .serialize()
    .expect("serialize psbt");

    let mut module = silent::new_keys_module(FfiNetwork::Regtest);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );

    let sp_fp_hex = sp_fp.to_string();
    let add_sp = module.add_key(&sp_fp_hex, "sp key");
    assert!(add_sp.is_ok, "add sp key: {}", add_sp.error);
    let sp_route = module.add_route(&sp_fp_hex, KeyRouteKind::Qr);
    assert!(sp_route.is_ok, "add sp route: {}", sp_route.error);
    let register = module.register_descriptor(&sp_fp_hex, &descriptor.to_string());
    assert!(register.is_ok, "register sp descriptor: {}", register.error);

    let other_fp_hex = other_fp.to_string();
    let add_other = module.add_key(&other_fp_hex, "other key");
    assert!(add_other.is_ok, "add other key: {}", add_other.error);
    let other_route = module.add_route(&other_fp_hex, KeyRouteKind::Qr);
    assert!(other_route.is_ok, "add other route: {}", other_route.error);

    let plan = module.signing_plan(&hex::encode(bytes));
    assert!(plan.is_ok, "signing_plan failed: {}", plan.error);
    assert!(
        plan.sp_outputs_pending,
        "expected sp_outputs_pending, got {plan:?}"
    );

    assert_eq!(
        plan.candidates[0].fingerprint, sp_fp_hex,
        "expected the sp key first, got {:?}",
        plan.candidates
    );
    assert!(
        plan.candidates[0].must_sign_first,
        "expected the sp candidate to have must_sign_first set, got {:?}",
        plan.candidates[0]
    );
    assert!(
        plan.candidates[0].can_sign,
        "expected the sp candidate to be able to sign, got {:?}",
        plan.candidates[0]
    );

    let other = plan
        .candidates
        .iter()
        .find(|candidate| candidate.fingerprint == other_fp_hex)
        .expect("other candidate present");
    assert!(
        !other.can_sign,
        "expected the other key to be blocked, got {other:?}"
    );
    assert_eq!(
        other.unusable,
        KeyUnusableReason::SpOutputsPending,
        "expected SpOutputsPending, got {:?}",
        other.unusable
    );
}

#[test]
fn test_plan_picks_the_second_keys_sp_descriptor_when_two_are_registered() {
    let _ = datadir();
    let secp = secp();
    let (first_descriptor, first_fp) = third_sp_descriptor_with_origin();
    let (second_descriptor, second_fp) = second_sp_descriptor_with_origin();
    let second_spend_pubkey = second_descriptor
        .spend_public_key(&secp)
        .expect("spend pubkey");

    let mut input = blank_input();
    set_sp_input_spend_bip32_derivation(
        &mut input.psbt,
        second_spend_pubkey,
        key_source(second_fp),
    );

    let mut output = blank_output(None);
    set_sp_v0_output(
        &mut output.psbt,
        public_key(&secp, 0xd1),
        public_key(&secp, 0xd2),
        None,
    );
    let bytes = one_input_psbt(input, output)
        .serialize()
        .expect("serialize psbt");

    let mut module = silent::new_keys_module(FfiNetwork::Regtest);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );

    let first_fp_hex = first_fp.to_string();
    let add_first = module.add_key(&first_fp_hex, "first key");
    assert!(add_first.is_ok, "add first key: {}", add_first.error);
    let first_route = module.add_route(&first_fp_hex, KeyRouteKind::Qr);
    assert!(first_route.is_ok, "add first route: {}", first_route.error);
    let register_first = module.register_descriptor(&first_fp_hex, &first_descriptor.to_string());
    assert!(
        register_first.is_ok,
        "register first descriptor: {}",
        register_first.error
    );

    let second_fp_hex = second_fp.to_string();
    let add_second = module.add_key(&second_fp_hex, "second key");
    assert!(add_second.is_ok, "add second key: {}", add_second.error);
    let second_route = module.add_route(&second_fp_hex, KeyRouteKind::Qr);
    assert!(
        second_route.is_ok,
        "add second route: {}",
        second_route.error
    );
    let register_second =
        module.register_descriptor(&second_fp_hex, &second_descriptor.to_string());
    assert!(
        register_second.is_ok,
        "register second descriptor: {}",
        register_second.error
    );

    let plan = module.signing_plan(&hex::encode(bytes));
    assert!(plan.is_ok, "signing_plan failed: {}", plan.error);
    assert_eq!(
        plan.candidates.len(),
        1,
        "expected only the second key's fingerprint to be required, the first key must \
         not be a signing candidate at all, got {:?}",
        plan.candidates
    );
    assert_eq!(
        plan.candidates[0].fingerprint, second_fp_hex,
        "expected the plan to pick the second key's sp descriptor, got {:?}",
        plan.candidates
    );
    assert!(
        plan.candidates[0].must_sign_first,
        "expected the second key to sign first, got {:?}",
        plan.candidates[0]
    );
}

// ===== Case 6: sp() descriptor origin validation =====

#[test]
fn test_sp_descriptor_without_origin_is_rejected() {
    let without_origin = silent::validate_sp_descriptor_origin(sp_descriptor_without_origin());
    assert!(
        !without_origin.is_ok,
        "expected an origin-less descriptor to be rejected"
    );
    assert!(
        without_origin.error.contains("origin"),
        "expected the error to name the missing origin, got {}",
        without_origin.error
    );
    assert!(
        without_origin.fingerprint.is_empty(),
        "expected an empty fingerprint on error, got {}",
        without_origin.fingerprint
    );

    let (with_origin, fp) = sp_descriptor_with_origin();
    let result = silent::validate_sp_descriptor_origin(with_origin.to_string());
    assert!(result.is_ok, "expected ok, got error: {}", result.error);
    assert_eq!(
        result.fingerprint,
        fp.to_string(),
        "expected the descriptor's own origin fingerprint, got {}",
        result.fingerprint
    );
}

// ===== Case 7: a usb-only key with no live device cannot sign =====

#[test]
fn test_usb_only_key_with_no_live_device_is_unusable() {
    let _ = datadir();
    let fp_hex = "55555555";

    let mut module = silent::new_keys_module(FfiNetwork::Testnet);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );
    let add = module.add_key(fp_hex, "usb only");
    assert!(add.is_ok, "add key: {}", add.error);
    let route = module.add_route(fp_hex, KeyRouteKind::Usb);
    assert!(route.is_ok, "add usb route: {}", route.error);

    let secp = secp();
    let mut input = blank_input();
    input.psbt.bip32_derivation.insert(
        public_key(&secp, 0x91),
        key_source(Fingerprint::from_str(fp_hex).expect("valid fingerprint")),
    );
    let bytes = one_input_psbt(input, blank_output(Some(ScriptBuf::new())))
        .serialize()
        .expect("serialize psbt");

    let plan = module.signing_plan(&hex::encode(bytes));
    assert!(plan.is_ok, "signing_plan failed: {}", plan.error);
    assert_eq!(
        plan.candidates.len(),
        1,
        "expected a single candidate, got {:?}",
        plan.candidates
    );
    assert!(
        !plan.candidates[0].can_sign,
        "expected the usb-only key to be unusable, got {:?}",
        plan.candidates[0]
    );
    assert_eq!(
        plan.candidates[0].unusable,
        KeyUnusableReason::DeviceNotConnected,
        "expected DeviceNotConnected, got {:?}",
        plan.candidates[0].unusable
    );
}

// ===== USB signing: v2 <-> v0 conversion and signature merge =====

const TEST_MNEMONIC: &str = "abandon abandon abandon abandon abandon abandon \
    abandon abandon abandon abandon abandon about";

const RECEIVE_INDEX: u32 = 0;

/// The account-level wpkh() descriptor derived from the test mnemonic, and the
/// origin path it was built at.
fn wpkh_descriptor(network: Network) -> (String, DerivationPath) {
    let account = ChildNumber::from_hardened_idx(0).expect("valid account index");
    let path = wpkh_path(network, account).expect("valid wpkh path");
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    let xpub = signer.xpub(&path);
    (wpkh(xpub).to_string(), path)
}

/// A v2 PSBT with one input spending the test mnemonic's account-level wpkh()
/// descriptor at receive index 0, carrying an SP input tweak and ECDH share
/// alongside the bip32_derivation and witness_utxo a hot signer needs to sign it.
/// The output script is already derived, so nothing here is pending.
fn usb_signable_psbt(network: Network) -> PsbtV2 {
    let (descriptor_string, path) = wpkh_descriptor(network);
    let descriptor =
        to_wallet(
            bwk_sp::bwk_sign::miniscript::Descriptor::<
                bwk_sp::bwk_sign::miniscript::DescriptorPublicKey,
            >::from_str(&descriptor_string)
            .expect("valid wpkh descriptor"),
        );
    let derivator = descriptor
        .spk_derivator(network)
        .expect("valid spk derivator");
    let spk = derivator.receive_spk_at(RECEIVE_INDEX);

    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    let full_path = path
        .child(ChildNumber::from_normal_idx(0).expect("receive branch"))
        .child(ChildNumber::from_normal_idx(RECEIVE_INDEX).expect("receive index"));
    let pubkey = signer.public_key_at(&full_path);

    let secp = secp();
    let mut input = blank_input();
    input
        .psbt
        .bip32_derivation
        .insert(pubkey, (signer.fingerprint(), full_path));
    input.psbt.witness_utxo = Some(TxOut {
        value: Amount::from_sat(100_000),
        script_pubkey: spk,
    });
    set_sp_input_tweak(&mut input.psbt, [0x22; 32]);
    set_sp_input_ecdh_share(
        &mut input.psbt,
        public_key(&secp, 0x33),
        public_key(&secp, 0x34),
    );

    one_input_psbt(input, blank_output(Some(ScriptBuf::new())))
}

#[test]
fn test_usb_sign_round_trip_keeps_sp_fields_and_merges_the_signature() {
    let network = Network::Bitcoin;
    let psbt = usb_signable_psbt(network);
    let psbt_v2_hex = hex::encode(psbt.serialize().expect("serialize psbt"));

    let payload = silent::usb_sign_payload(&psbt_v2_hex, "", false);
    assert!(payload.is_ok, "expected ok, got error: {}", payload.error);

    let mut v0 = Psbt::from_str(&payload.psbt_base64).expect("valid v0 psbt");
    let (descriptor_string, _) = wpkh_descriptor(network);
    let descriptor =
        to_wallet(
            bwk_sp::bwk_sign::miniscript::Descriptor::<
                bwk_sp::bwk_sign::miniscript::DescriptorPublicKey,
            >::from_str(&descriptor_string)
            .expect("valid wpkh descriptor"),
        );
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    signer
        .inner_sign(&mut v0, &descriptor)
        .expect("sign succeeds");
    assert!(
        !v0.inputs[0].partial_sigs.is_empty(),
        "expected a partial signature on the signed v0 psbt"
    );

    let signed_base64 = v0.to_string();
    let merged = silent::usb_merge_signed(&psbt_v2_hex, &signed_base64);
    assert!(merged.is_ok, "expected ok, got error: {}", merged.error);

    let merged_bytes = hex::decode(&merged.psbt_v2_hex).expect("valid hex");
    let merged_psbt = PsbtV2::deserialize(&merged_bytes).expect("merged psbt parses as v2");
    assert!(
        !merged_psbt.inputs[0].psbt.partial_sigs.is_empty(),
        "expected the merged psbt to carry the new signature"
    );
    assert_eq!(
        sp_input_tweak(&merged_psbt.inputs[0].psbt).expect("valid tweak field"),
        Some([0x22; 32]),
        "expected the sp input tweak to survive the round trip"
    );
    let secp = secp();
    assert_eq!(
        sp_input_ecdh_share(&merged_psbt.inputs[0].psbt, public_key(&secp, 0x33))
            .expect("valid ecdh share field"),
        Some(public_key(&secp, 0x34)),
        "expected the sp ecdh share to survive the round trip"
    );
}

#[test]
fn test_merge_signatures_rejects_a_signed_psbt_for_a_different_transaction() {
    let network = Network::Testnet;
    let original = usb_signable_psbt(network);
    let original_bytes = original.serialize().expect("serialize original");

    let mut other_input = blank_input();
    other_input.previous_output = OutPoint::new(Txid::from_byte_array([0x44; 32]), 0);
    let other_psbt = one_input_psbt(other_input, blank_output(Some(ScriptBuf::new())));
    let signed_bytes = other_psbt.into_bitcoin_psbt().expect("into v0").serialize();

    let result = merge_signatures_bytes(&original_bytes, &signed_bytes);
    assert!(
        matches!(result, Err(Error::SignedPsbtMismatch { .. })),
        "expected SignedPsbtMismatch, got {result:?}"
    );
}

#[test]
fn test_merge_signatures_with_no_new_signature_errors() {
    let network = Network::Signet;
    let original = usb_signable_psbt(network);
    let original_bytes = original.serialize().expect("serialize original");

    // An unsigned v0 psbt for the very same transaction: same txid, no signature.
    let unsigned_bytes = original
        .clone()
        .into_bitcoin_psbt()
        .expect("into v0")
        .serialize();

    let result = merge_signatures_bytes(&original_bytes, &unsigned_bytes);
    assert!(
        matches!(result, Err(Error::NothingSigned)),
        "expected NothingSigned, got {result:?}"
    );
}

#[test]
fn test_usb_sign_payload_refuses_a_pending_sp_output() {
    let psbt = sp_pending_output_psbt();
    let bytes = psbt.serialize().expect("serialize psbt");

    let payload = silent::usb_sign_payload(&hex::encode(bytes), "", false);
    assert!(!payload.is_ok, "expected a pending sp output to be refused");
    assert!(
        payload.error.contains("sp()"),
        "expected the sp-outputs-pending error, got {}",
        payload.error
    );
    assert!(
        payload.psbt_base64.is_empty(),
        "expected no payload on error, got {}",
        payload.psbt_base64
    );
}
