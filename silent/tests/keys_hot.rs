//! The hot key path: generation, import, reveal, and signing.
//!
//! Every case goes through `KeysModule`, which reads the process-wide datadir on
//! open, so the whole file shares one datadir and each case picks its own network to
//! keep the standard test mnemonic's fingerprint from colliding with itself.

use std::str::FromStr;

use bitcoin::{
    bip32::{ChildNumber, DerivationPath, Fingerprint, Xpriv, Xpub},
    secp256k1::{PublicKey, Secp256k1, SecretKey},
    Amount, Network, NetworkKind, OutPoint, Psbt, ScriptBuf, Sequence, TxOut,
};
use bwk_sp::{
    bwk_sign::{
        bwk_descriptor::{
            descriptor::{wpkh, wpkh_path, Descriptor, DescriptorDerivator},
            updater::update_input,
            wallet::{to_wallet, WalletDescriptorExt},
        },
        bwk_keys::keys::OXpub,
        hot_signer::HotSigner,
    },
    bwk_tx::bwk_psbt::{
        musig::{musig_input_partial_sigs, musig_input_pub_nonces, set_musig_input_pub_nonce},
        Input as PsbtInput, Output as PsbtOutput, PsbtV2,
    },
};
use silent::{KeyRouteKind, LiveDevice, Network as FfiNetwork};

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

// ===== Case 19: hot_reveal and hot_sp_descriptor succeed with the right password, flag the wrong one =====

#[test]
fn test_hot_reveal_and_sp_descriptor_with_right_and_wrong_password() {
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
    assert!(
        !right.wrong_password,
        "expected no wrong password flag on success"
    );
    assert_eq!(
        right.mnemonic, TEST_MNEMONIC,
        "expected the exact imported mnemonic, got {}",
        right.mnemonic
    );

    let wrong = silent::hot_reveal(&material, "wrong password");
    assert!(!wrong.is_ok, "expected a wrong password to be refused");
    assert!(
        wrong.wrong_password,
        "expected the wrong password flag, got error: {}",
        wrong.error
    );
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

    let descriptor = silent::hot_sp_descriptor(&material, "password");
    assert!(
        descriptor.is_ok,
        "expected ok, got error: {}",
        descriptor.error
    );
    assert!(
        !descriptor.wrong_password,
        "expected no wrong password flag on success"
    );

    let refused = silent::hot_sp_descriptor(&material, "wrong password");
    assert!(!refused.is_ok, "expected a wrong password to be refused");
    assert!(
        refused.wrong_password,
        "expected the wrong password flag, got error: {}",
        refused.error
    );
    assert!(
        refused.descriptor.is_empty(),
        "expected no descriptor on a refused fetch, got {}",
        refused.descriptor
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
        version: String::new(),
        capabilities: 0,
    }]);

    let psbt_hex = hex::encode(
        bip32_psbt(Network::Signet)
            .serialize()
            .expect("serialize psbt"),
    );
    let wrong = silent::hot_sign(&material, "wrong password", &psbt_hex);
    assert!(!wrong.is_ok, "expected a wrong password to be refused");
    assert!(
        wrong.wrong_password,
        "expected the wrong password flag, got error: {}",
        wrong.error
    );

    let result = silent::hot_sign(&material, "password", &psbt_hex);
    assert!(result.is_ok, "expected ok, got error: {}", result.error);
    assert!(
        !result.wrong_password,
        "expected no wrong password flag on success"
    );

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
        !result.wrong_password,
        "expected a right password not to be flagged, got error: {}",
        result.error
    );
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

// ===== MuSig2 fixtures =====

/// Not TEST_MNEMONIC: every network of this file already holds its hot secret, and
/// a second commit of it would be refused.
const MUSIG_MNEMONIC: &str = "legal winner thank year wave sausage worth useful \
    legal winner thank yellow";
const MUSIG_PATH: &str = "m/87'/1'/0'/2";
const COSIGNER_SEED: [u8; 32] = [0x42; 32];
const COSIGNER_ORIGIN: [u8; 4] = [0xbb, 0x00, 0x00, 0x02];
const PUB_NONCE_LEN: usize = 66;

/// The hot key's `[fp/87'/1'/0'/2]` key.
fn hot_xpub(network: Network) -> OXpub {
    let path = DerivationPath::from_str(MUSIG_PATH).expect("valid path");
    let signer = HotSigner::new_from_mnemonics(network, MUSIG_MNEMONIC).expect("valid signer");
    signer.xpub(&path)
}

fn cosigner_xpub() -> Xpub {
    let xpriv = Xpriv::new_master(NetworkKind::Test, &COSIGNER_SEED).expect("valid seed");
    Xpub::from_priv(&Secp256k1::new(), &xpriv)
}

/// `tr(musig(H, B)/<0;1>/*)`: the participants are both xpubs' own keys.
fn musig_descriptor(network: Network) -> String {
    let origin = Fingerprint::from(COSIGNER_ORIGIN);
    format!(
        "tr(musig({},[{origin}]{})/<0;1>/*)",
        hot_xpub(network),
        cosigner_xpub()
    )
}

/// A v2 PSBT spending `descriptor` at index 0, filled by the bwk updater.
fn musig_psbt(descriptor: &str) -> PsbtV2 {
    let parsed = Descriptor::from_str(descriptor).expect("valid musig descriptor");
    let wallet = parsed.as_miniscript().expect("a miniscript descriptor");
    let definite = wallet.single_paths().expect("single paths")[0]
        .derive_at(RECEIVE_INDEX)
        .expect("derive index 0");
    let tx = bitcoin::Transaction {
        version: bitcoin::transaction::Version::TWO,
        lock_time: bitcoin::absolute::LockTime::ZERO,
        input: vec![bitcoin::TxIn {
            previous_output: OutPoint::new(bitcoin::hashes::Hash::all_zeros(), 0),
            sequence: Sequence::MAX,
            ..Default::default()
        }],
        output: vec![TxOut {
            value: Amount::from_sat(1_000),
            script_pubkey: ScriptBuf::new(),
        }],
    };
    let mut psbt = Psbt::from_unsigned_tx(tx).expect("unsigned tx");
    psbt.inputs[0].witness_utxo = Some(TxOut {
        value: Amount::from_sat(100_000),
        script_pubkey: definite.script_pubkey(),
    });
    update_input(&mut psbt, 0, &definite).expect("update input");
    PsbtV2::from_bitcoin_psbt(psbt).expect("psbt to v2")
}

/// A valid 66-byte public nonce: two compressed public keys of fixed secrets.
fn pub_nonce(first: u8, second: u8) -> [u8; PUB_NONCE_LEN] {
    let secp = Secp256k1::new();
    let point = |byte: u8| {
        let secret = SecretKey::from_slice(&[byte; 32]).expect("valid key");
        PublicKey::from_secret_key(&secp, &secret).serialize()
    };
    let mut nonce = [0u8; PUB_NONCE_LEN];
    nonce[..33].copy_from_slice(&point(first));
    nonce[33..].copy_from_slice(&point(second));
    nonce
}

/// Commits the musig hot key on `network`, registers the musig descriptor on it and
/// runs round 1: the material and the PSBT round 1 returned.
fn hot_round1(network: FfiNetwork, btc_network: Network) -> (Box<silent::HotMaterial>, PsbtV2) {
    let _ = datadir();
    let mut module = silent::new_keys_module(network);
    assert!(
        module.is_ok(),
        "module failed to open: {}",
        module.get_error()
    );
    let committed = seal_and_commit(&mut module, MUSIG_MNEMONIC, "password", "musig", network);
    assert!(committed.is_ok, "commit failed: {}", committed.error);
    let fingerprint = committed.key.fingerprint.clone();

    let descriptor = musig_descriptor(btc_network);
    let register = module.register_descriptor(&fingerprint, &descriptor);
    assert!(register.is_ok, "register failed: {}", register.error);

    let material = module.hot_material(&fingerprint);
    assert!(
        material.is_ok(),
        "hot_material failed: {}",
        material.get_error()
    );
    let psbt_hex = hex::encode(musig_psbt(&descriptor).serialize().expect("serialize psbt"));
    let result = silent::hot_sign(&material, "password", &psbt_hex);
    assert!(
        result.is_ok,
        "expected round 1 ok, got error: {}",
        result.error
    );
    let signed = PsbtV2::deserialize(&hex::decode(&result.psbt_v2_hex).expect("valid hex"))
        .expect("round 1 psbt parses as v2");
    (material, signed)
}

// ===== Case 24: the hot key's round 1 adds only its nonce =====

#[test]
fn hot_round1_adds_only_its_nonce() {
    let (_, signed) = hot_round1(FfiNetwork::Regtest, Network::Regtest);
    let input = &signed.inputs[0].psbt;
    let participants: Vec<PublicKey> = musig_input_pub_nonces(input)
        .expect("valid 0x1b entries")
        .into_iter()
        .map(|nonce| nonce.participant)
        .collect();
    let hot = hot_xpub(Network::Regtest).xkey.public_key;
    assert_eq!(
        participants,
        vec![hot],
        "expected exactly the hot key's 0x1b"
    );
    assert!(
        input.tap_key_sig.is_none(),
        "expected no tap_key_sig in round 1"
    );
    let sigs = musig_input_partial_sigs(input).expect("valid 0x1c entries");
    assert!(sigs.is_empty(), "expected no 0x1c in round 1, got {sigs:?}");
}

// ===== Case 25: round 2 against a nonce the session never gave is a session error =====

#[test]
fn hot_round2_without_its_session_is_musig_session() {
    let (material, mut psbt) = hot_round1(FfiNetwork::Signet, Network::Signet);
    let input = &mut psbt.inputs[0].psbt;
    let mut nonce = musig_input_pub_nonces(input).expect("valid 0x1b entries")[0];
    nonce.nonce = pub_nonce(0x21, 0x22);
    set_musig_input_pub_nonce(input, &nonce);
    nonce.participant = cosigner_xpub().public_key;
    nonce.nonce = pub_nonce(0x31, 0x32);
    set_musig_input_pub_nonce(input, &nonce);

    let psbt_hex = hex::encode(psbt.serialize().expect("serialize psbt"));
    let result = silent::hot_sign(&material, "password", &psbt_hex);
    assert!(!result.is_ok, "expected round 2 refused");
    assert!(
        result.musig_session,
        "expected the musig session flag, got error: {}",
        result.error
    );
    assert!(
        !result.wrong_password,
        "expected no wrong password flag, got error: {}",
        result.error
    );
}
