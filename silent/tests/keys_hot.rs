//! The hot key path: generation, import, reveal, and signing.
//!
//! Every case goes through `KeysModule`, which reads the process-wide datadir on
//! open, so the whole file shares one datadir and each case picks its own network to
//! keep the standard test mnemonic's fingerprint from colliding with itself.

use std::str::FromStr;

use bitcoin::bip32::{ChildNumber, Fingerprint};
use bitcoin::{Amount, Network, OutPoint, ScriptBuf, Sequence, TxOut};
use bwk_sp::bwk_sign::bwk_descriptor::descriptor::{wpkh, wpkh_path, DescriptorDerivator};
use bwk_sp::bwk_sign::hot_signer::HotSigner;
use bwk_sp::bwk_tx::bwk_psbt::{Input as PsbtInput, Output as PsbtOutput, PsbtV2};
use silent::{KeyRouteKind, LiveDevice, Network as FfiNetwork};

// ===== Datadir =====

static DATADIR: std::sync::OnceLock<temp_dir::TempDir> = std::sync::OnceLock::new();

fn datadir() -> &'static std::path::Path {
    DATADIR
        .get_or_init(|| {
            let tmp = temp_dir::TempDir::new().expect("create temp dir");
            silent::set_datadir(tmp.path().to_path_buf());
            tmp
        })
        .path()
}

// ===== Fixtures =====

const TEST_MNEMONIC: &str = "abandon abandon abandon abandon abandon abandon \
    abandon abandon abandon abandon abandon about";
const TEST_FINGERPRINT_HEX: &str = "73c5da0a";

const RECEIVE_INDEX: u32 = 0;

/// The account-level wpkh() descriptor derived from the test mnemonic, and the
/// origin path it was built at. `network` only picks the coin type embedded in the
/// origin path (a label); the derived key material is the same regardless, so a
/// caller only has to keep it consistent with the module's own network kind (main
/// vs. test) it will later ask to sign with.
fn wpkh_descriptor(network: Network) -> (String, bitcoin::bip32::DerivationPath) {
    let account = ChildNumber::from_hardened_idx(0).expect("valid account index");
    let path = wpkh_path(network, account).expect("valid wpkh path");
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    let xpub = signer.xpub(&path);
    (wpkh(xpub).to_string(), path)
}

fn blank_input() -> PsbtInput {
    PsbtInput {
        previous_output: OutPoint::new(bitcoin::hashes::Hash::all_zeros(), 0),
        sequence: Sequence::MAX,
        required_time_lock_time: None,
        required_height_lock_time: None,
        psbt: bitcoin::psbt::Input::default(),
    }
}

fn blank_output() -> PsbtOutput {
    PsbtOutput {
        amount: Amount::from_sat(1_000),
        script_pubkey: Some(ScriptBuf::new()),
        psbt: bitcoin::psbt::Output::default(),
    }
}

fn one_input_psbt(input: PsbtInput) -> PsbtV2 {
    PsbtV2 {
        tx_version: bitcoin::transaction::Version::TWO,
        fallback_lock_time: Some(bitcoin::absolute::LockTime::ZERO),
        tx_modifiable: None,
        xpub: Default::default(),
        proprietary: Default::default(),
        unknown: Default::default(),
        inputs: vec![input],
        outputs: vec![blank_output()],
    }
}

/// A v2 PSBT with one input spending the test mnemonic's account-level wpkh()
/// descriptor at receive index 0, with a `witness_utxo` and the matching
/// `bip32_derivation`, ready for `sign_hot` to sign.
fn bip32_psbt(network: Network) -> PsbtV2 {
    let (descriptor_string, path) = wpkh_descriptor(network);
    let descriptor = bwk_sp::bwk_sign::miniscript::Descriptor::<
        bwk_sp::bwk_sign::miniscript::DescriptorPublicKey,
    >::from_str(&descriptor_string)
    .expect("valid wpkh descriptor");
    let derivator = descriptor
        .spk_derivator(network)
        .expect("valid spk derivator");
    let spk = derivator.receive_spk_at(RECEIVE_INDEX);

    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    let full_path = path
        .child(ChildNumber::from_normal_idx(0).expect("receive branch"))
        .child(ChildNumber::from_normal_idx(RECEIVE_INDEX).expect("receive index"));
    let pubkey = signer.public_key_at(&full_path);

    let mut input = blank_input();
    input
        .psbt
        .bip32_derivation
        .insert(pubkey, (signer.fingerprint(), full_path));
    input.psbt.witness_utxo = Some(TxOut {
        value: Amount::from_sat(100_000),
        script_pubkey: spk,
    });

    one_input_psbt(input)
}

/// A v2 PSBT whose input carries a `bip32_derivation` entry for a fingerprint the
/// registered wpkh() descriptor knows nothing about, so nothing on it can sign.
fn foreign_descriptor_psbt() -> PsbtV2 {
    let secp = bitcoin::secp256k1::Secp256k1::new();
    let pubkey = bitcoin::secp256k1::PublicKey::from_secret_key(
        &secp,
        &bitcoin::secp256k1::SecretKey::from_slice(&[0x77; 32]).expect("valid key"),
    );
    let foreign_fp = Fingerprint::from([0x99u8, 0x99, 0x99, 0x99]);
    let path = bitcoin::bip32::DerivationPath::from(vec![
        ChildNumber::from_normal_idx(0).expect("index"),
        ChildNumber::from_normal_idx(0).expect("index"),
    ]);

    let mut input = blank_input();
    input
        .psbt
        .bip32_derivation
        .insert(pubkey, (foreign_fp, path));
    input.psbt.witness_utxo = Some(TxOut {
        value: Amount::from_sat(100_000),
        script_pubkey: ScriptBuf::new(),
    });

    one_input_psbt(input)
}

// ===== Seal/commit helper =====

/// Seals `mnemonic` under `password` for `network` and commits it to `module` under
/// `alias`, asserting the seal itself succeeds. Mirrors the GUI-copy/worker/commit
/// split the C++ side drives across a QThread boundary: `hot_seal` here plays the
/// worker's part, `commit_hot_key` the GUI thread's.
fn seal_and_commit(
    module: &mut Box<silent::KeysModule>,
    mnemonic: &str,
    password: &str,
    alias: &str,
    network: FfiNetwork,
) -> silent::KeyInfoResult {
    let sealed = silent::hot_seal(mnemonic, password, network);
    assert!(sealed.is_ok(), "seal failed: {}", sealed.get_error());
    module.commit_hot_key(&sealed, alias)
}

// ===== Case 17: generation works on the network a hot key must not panic on =====

#[test]
fn test_generate_mnemonic_on_mainnet() {
    let _ = datadir();
    let mut module = silent::new_keys_module(FfiNetwork::Bitcoin);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );

    let twelve = silent::generate_mnemonic(12);
    assert!(twelve.is_ok, "expected ok, got error: {}", twelve.error);
    let word_count = twelve.mnemonic.split_whitespace().count();
    assert_eq!(word_count, 12, "expected 12 words, got {word_count}");

    let twenty_four = silent::generate_mnemonic(24);
    assert!(
        twenty_four.is_ok,
        "expected ok, got error: {}",
        twenty_four.error
    );
    let word_count = twenty_four.mnemonic.split_whitespace().count();
    assert_eq!(word_count, 24, "expected 24 words, got {word_count}");

    let thirteen = silent::generate_mnemonic(13);
    assert!(
        !thirteen.is_ok,
        "expected an unsupported word count to error"
    );

    // Sealing and committing on mainnet exercises the whole hot-key path with the
    // freshly generated mnemonic: `HotSigner::new` would assert here, so this
    // proves `generate_mnemonic` stays on the assert-free `new_from_mnemonics` path.
    let committed = seal_and_commit(
        &mut module,
        &twelve.mnemonic,
        "password",
        "alias",
        FfiNetwork::Bitcoin,
    );
    assert!(
        committed.is_ok,
        "expected the generated mnemonic to commit on mainnet, got error: {}",
        committed.error
    );
}

// ===== Case 18: seal then commit creates the record with the hot route from the start =====

#[test]
fn test_seal_then_commit_creates_the_record_with_hot_route_from_the_start() {
    let _ = datadir();
    let mut module = silent::new_keys_module(FfiNetwork::Regtest);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );

    let committed = seal_and_commit(
        &mut module,
        TEST_MNEMONIC,
        "password",
        "alias",
        FfiNetwork::Regtest,
    );
    assert!(
        committed.is_ok,
        "expected ok, got error: {}",
        committed.error
    );
    assert_eq!(
        committed.key.fingerprint, TEST_FINGERPRINT_HEX,
        "expected the standard test vector's fingerprint, got {}",
        committed.key.fingerprint
    );
    assert!(
        committed.key.has_hot_secret,
        "expected the committed key to carry a secret"
    );
    assert_eq!(
        committed.key.routes.first().copied(),
        Some(KeyRouteKind::Hot),
        "expected Hot at position 0, got {:?}",
        committed.key.routes
    );
}

// ===== Case 19: hot_material + hot_reveal succeeds with the right password, fails with the wrong one =====

#[test]
fn test_hot_material_reveal_with_right_and_wrong_password() {
    let _ = datadir();
    let mut module = silent::new_keys_module(FfiNetwork::Testnet);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );
    let committed = seal_and_commit(
        &mut module,
        TEST_MNEMONIC,
        "password",
        "alias",
        FfiNetwork::Testnet,
    );
    assert!(committed.is_ok, "commit failed: {}", committed.error);

    let material = module.hot_material(TEST_FINGERPRINT_HEX);
    assert!(
        material.is_ok(),
        "hot_material failed: {}",
        material.get_error()
    );

    let right = silent::hot_reveal(&material, "password");
    assert!(right.is_ok, "expected ok, got error: {}", right.error);
    assert_eq!(
        right.mnemonic, TEST_MNEMONIC,
        "expected the exact imported mnemonic, got {}",
        right.mnemonic
    );

    let wrong = silent::hot_reveal(&material, "wrong password");
    assert!(!wrong.is_ok, "expected a wrong password to be refused");
    assert!(
        wrong.mnemonic.is_empty(),
        "expected an empty mnemonic on a refused reveal, got {}",
        wrong.mnemonic
    );
    assert!(
        !wrong.error.to_lowercase().contains("abandon"),
        "expected no fragment of the phrase in the error, got {}",
        wrong.error
    );
}

// ===== Case 20: hot_material + hot_sign lands a signature, independent of a later module mutation =====

#[test]
fn test_hot_material_sign_is_independent_of_a_later_module_mutation() {
    let _ = datadir();
    let mut module = silent::new_keys_module(FfiNetwork::Signet);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );
    let committed = seal_and_commit(
        &mut module,
        TEST_MNEMONIC,
        "password",
        "alias",
        FfiNetwork::Signet,
    );
    assert!(committed.is_ok, "commit failed: {}", committed.error);

    let (descriptor_string, _) = wpkh_descriptor(Network::Signet);
    let register = module.register_descriptor(TEST_FINGERPRINT_HEX, &descriptor_string);
    assert!(register.is_ok, "register failed: {}", register.error);

    let material = module.hot_material(TEST_FINGERPRINT_HEX);
    assert!(
        material.is_ok(),
        "hot_material failed: {}",
        material.get_error()
    );

    // Mutates the module between the copy-out and the sign call: `material` must be
    // unaffected, proving `hot_sign` never touches the module it was copied from.
    module.set_live_devices(vec![LiveDevice {
        device_id: "dev-1".to_string(),
        fingerprint: String::new(),
        kind: "hardware".to_string(),
        brand: String::new(),
        model: "model".to_string(),
        serial: "serial".to_string(),
        module: "hwi".to_string(),
    }]);

    let psbt_hex = hex::encode(
        bip32_psbt(Network::Signet)
            .serialize()
            .expect("serialize psbt"),
    );
    let result = silent::hot_sign(&material, "password", &psbt_hex);
    assert!(result.is_ok, "expected ok, got error: {}", result.error);

    let signed_bytes = hex::decode(&result.psbt_v2_hex).expect("valid hex");
    let signed = PsbtV2::deserialize(&signed_bytes).expect("signed hex parses as psbt v2");
    assert!(
        !signed.inputs[0].psbt.partial_sigs.is_empty(),
        "expected a partial signature on input 0, got {:?}",
        signed.inputs[0].psbt.partial_sigs
    );
}

// ===== Case 21: a psbt with no matching descriptor signs nothing =====

#[test]
fn test_hot_sign_with_no_matching_descriptor_returns_nothing_signed() {
    let _ = datadir();
    let mut module = silent::new_keys_module(FfiNetwork::Bitcoin);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );
    let committed = seal_and_commit(
        &mut module,
        TEST_MNEMONIC,
        "password",
        "alias",
        FfiNetwork::Bitcoin,
    );
    assert!(committed.is_ok, "commit failed: {}", committed.error);

    let (descriptor_string, _) = wpkh_descriptor(Network::Bitcoin);
    let register = module.register_descriptor(TEST_FINGERPRINT_HEX, &descriptor_string);
    assert!(register.is_ok, "register failed: {}", register.error);

    let material = module.hot_material(TEST_FINGERPRINT_HEX);
    assert!(
        material.is_ok(),
        "hot_material failed: {}",
        material.get_error()
    );

    let psbt_hex = hex::encode(
        foreign_descriptor_psbt()
            .serialize()
            .expect("serialize psbt"),
    );
    let result = silent::hot_sign(&material, "password", &psbt_hex);

    assert!(!result.is_ok, "expected the sign to fail");
    assert!(
        result.error.to_lowercase().contains("signed"),
        "expected the error to say nothing was signed, got {}",
        result.error
    );
    assert!(
        result.psbt_v2_hex.is_empty(),
        "expected no psbt bytes handed back on failure, got {}",
        result.psbt_v2_hex
    );
}

// ===== Case 22: origin validation refuses a spend descriptor =====

#[test]
fn test_validate_sp_descriptor_origin_refuses_a_spend_descriptor() {
    use bwk_sp::bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor;
    use bwk_sp::bwk_sign::bwk_descriptor::sp_key::{SpKey, SpSpendKey};
    use bwk_sp::receiver::{bip352_base_derivation, scan_path, spend_path};

    let network = Network::Regtest;
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    let account = ChildNumber::from_hardened_idx(0).expect("valid account index");
    let origin = (
        signer.fingerprint(),
        bitcoin::bip32::DerivationPath::from(bip352_base_derivation(network, account)),
    );
    let spend_descriptor = SpDescriptor::Packed {
        origin: Some(origin),
        key: SpKey::Spend(SpSpendKey {
            scan_key: signer.private_key_at(&scan_path(network, account)),
            spend_key: signer.private_key_at(&spend_path(network, account)),
            network: bitcoin::NetworkKind::Test,
        }),
    }
    .to_string();

    let result = silent::validate_sp_descriptor_origin(spend_descriptor);
    assert!(!result.is_ok, "expected a spend descriptor to be refused");
    assert!(
        result.error.contains("watch-only"),
        "expected the watch-only refusal message, got {}",
        result.error
    );
}

// ===== Case 23: commit_hot_key refuses a sealed key whose network does not match =====

#[test]
fn test_commit_hot_key_refuses_a_network_mismatch() {
    let _ = datadir();
    let mut module = silent::new_keys_module(FfiNetwork::Bitcoin);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );

    // A freshly generated mnemonic, not the shared TEST_MNEMONIC: this test's
    // fingerprint must not collide with the ones the cases above commit to disk.
    let generated = silent::generate_mnemonic(12);
    assert!(generated.is_ok, "generate failed: {}", generated.error);

    let sealed = silent::hot_seal(&generated.mnemonic, "password", FfiNetwork::Testnet);
    assert!(sealed.is_ok(), "seal failed: {}", sealed.get_error());
    let fingerprint = sealed.fingerprint();

    let result = module.commit_hot_key(&sealed, "alias");
    assert!(!result.is_ok, "expected a network mismatch to be refused");
    assert!(
        result.error.to_lowercase().contains("network"),
        "expected a network-mismatch error, got {}",
        result.error
    );

    let lookup = module.key(&fingerprint);
    assert!(
        !lookup.is_ok,
        "expected no key to be created for a refused network mismatch"
    );
}
