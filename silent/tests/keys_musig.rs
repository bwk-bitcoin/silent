//! MuSig2 PSBTs through the keys module: the required signers, the round helpers,
//! the signature merge, a QR round 1 request, the 0x0D response error and the USB
//! round 1 payload.
//!
//! None of these cases touch a key record, so no datadir override is needed here.

use std::collections::BTreeSet;

use base64::{prelude::BASE64_STANDARD, Engine};
use bitcoin::{
    absolute::LockTime,
    bip32::{DerivationPath, Fingerprint},
    hashes::{hash160, Hash},
    secp256k1::{PublicKey, Secp256k1, SecretKey},
    transaction, Amount, OutPoint, Psbt, ScriptBuf, Sequence,
};
use bwk_qr_protocol::{request, response, Message, MessageType, Response};
use bwk_sp::bwk_tx::bwk_psbt::{
    musig::{
        musig_input_partial_sigs, musig_input_participants_all, musig_input_pub_nonces,
        set_musig_input_partial_sig, set_musig_input_participants, set_musig_input_pub_nonce,
        MusigPartialSig, MusigPubNonce,
    },
    Input as PsbtInput, Output as PsbtOutput, PsbtV2,
};
use silent::{
    keys::{
        psbt::{
            add_internal_participants, merge_signatures_bytes, musig_done_fingerprints,
            musig_nonces_complete, required_fingerprints, strip_musig_round1,
        },
        qr::{QrResponse, QrSession},
        Error,
    },
    QrErrorKind,
};

// ===== Keys =====

const SECRET_A: u8 = 0x0a;
const SECRET_B: u8 = 0x0b;
const SECRET_AGGREGATE: u8 = 0x0c;
const FINGERPRINT_A: [u8; 4] = [0xaa, 0x00, 0x00, 0x01];
const FINGERPRINT_B: [u8; 4] = [0xbb, 0x00, 0x00, 0x02];
const NONCE_LEN: usize = 66;
const PARTIAL_SIG_LEN: usize = 32;

fn public_key(byte: u8) -> PublicKey {
    let secret = SecretKey::from_slice(&[byte; 32]).expect("valid secret key");
    PublicKey::from_secret_key(&Secp256k1::new(), &secret)
}

fn key_a() -> PublicKey {
    public_key(SECRET_A)
}

fn key_b() -> PublicKey {
    public_key(SECRET_B)
}

fn aggregate() -> PublicKey {
    public_key(SECRET_AGGREGATE)
}

fn fingerprint_a() -> Fingerprint {
    Fingerprint::from(FINGERPRINT_A)
}

fn fingerprint_b() -> Fingerprint {
    Fingerprint::from(FINGERPRINT_B)
}

/// The BIP-328 synthetic xpub fingerprint of the aggregate.
fn synthetic_fingerprint() -> Fingerprint {
    let id = hash160::Hash::hash(&aggregate().serialize()).to_byte_array();
    Fingerprint::from([id[0], id[1], id[2], id[3]])
}

// ===== PSBT fixtures =====

fn blank_input() -> PsbtInput {
    PsbtInput {
        previous_output: OutPoint::new(Hash::all_zeros(), 0),
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
        tx_version: transaction::Version::TWO,
        fallback_lock_time: Some(LockTime::ZERO),
        tx_modifiable: None,
        xpub: Default::default(),
        proprietary: Default::default(),
        unknown: Default::default(),
        inputs: vec![input],
        outputs: vec![blank_output()],
    }
}

/// A one-input PSBT with no musig field at all.
fn plain_psbt() -> PsbtV2 {
    one_input_psbt(blank_input())
}

fn add_origin(input: &mut PsbtInput, key: PublicKey, fingerprint: Fingerprint) {
    input.psbt.tap_key_origins.insert(
        key.x_only_public_key().0,
        (Vec::new(), (fingerprint, DerivationPath::master())),
    );
}

/// A one-input PSBT whose input is a musig spend of `aggregate -> [A, B]`, with A's
/// and B's origins and the aggregate's under its synthetic fingerprint.
fn musig_psbt() -> PsbtV2 {
    let mut input = blank_input();
    set_musig_input_participants(&mut input.psbt, aggregate(), &[key_a(), key_b()]);
    add_origin(&mut input, key_a(), fingerprint_a());
    add_origin(&mut input, key_b(), fingerprint_b());
    add_origin(&mut input, aggregate(), synthetic_fingerprint());
    one_input_psbt(input)
}

fn add_nonce(psbt: &mut PsbtV2, participant: PublicKey, byte: u8) {
    set_musig_input_pub_nonce(
        &mut psbt.inputs[0].psbt,
        &MusigPubNonce {
            participant,
            aggregate: aggregate(),
            leaf_hash: None,
            nonce: [byte; NONCE_LEN],
        },
    );
}

fn add_partial_sig(psbt: &mut PsbtV2, participant: PublicKey, byte: u8) {
    set_musig_input_partial_sig(
        &mut psbt.inputs[0].psbt,
        &MusigPartialSig {
            participant,
            aggregate: aggregate(),
            leaf_hash: None,
            signature: [byte; PARTIAL_SIG_LEN],
        },
    );
}

/// Every 0x1b of the first input, as (participant, nonce first byte).
fn nonces_of(input: &bitcoin::psbt::Input) -> BTreeSet<(PublicKey, u8)> {
    musig_input_pub_nonces(input)
        .expect("valid 0x1b entries")
        .into_iter()
        .map(|nonce| (nonce.participant, nonce.nonce[0]))
        .collect()
}

fn partial_sig_count(input: &bitcoin::psbt::Input) -> usize {
    musig_input_partial_sigs(input)
        .expect("valid 0x1c entries")
        .len()
}

fn v0_bytes(psbt: &PsbtV2) -> Vec<u8> {
    psbt.clone()
        .into_bitcoin_psbt()
        .expect("convert to v0")
        .serialize()
}

fn v2_bytes(psbt: &PsbtV2) -> Vec<u8> {
    psbt.serialize().expect("serialize v2")
}

// ===== Required signers =====

#[test]
fn required_fingerprints_skip_the_aggregate() {
    let required = required_fingerprints(&musig_psbt()).expect("required fingerprints");
    let expected = BTreeSet::from([fingerprint_a(), fingerprint_b()]);
    assert_eq!(
        required,
        expected,
        "expected the cosigners only, not the synthetic {}, got {required:?}",
        synthetic_fingerprint()
    );
}

// ===== Round helpers =====

#[test]
fn nonces_complete_counts_every_participant() {
    let mut psbt = musig_psbt();
    assert!(
        !musig_nonces_complete(&psbt).expect("no nonce"),
        "expected incomplete with no nonce"
    );

    add_nonce(&mut psbt, key_a(), 1);
    assert!(
        !musig_nonces_complete(&psbt).expect("A's nonce"),
        "expected incomplete with A's nonce only"
    );

    add_nonce(&mut psbt, key_b(), 2);
    assert!(
        musig_nonces_complete(&psbt).expect("both nonces"),
        "expected complete with A's and B's nonces"
    );

    assert!(
        musig_nonces_complete(&plain_psbt()).expect("no 0x1a"),
        "expected complete for a PSBT with no 0x1a"
    );
}

#[test]
fn strip_round1_drops_every_musig_field() {
    let mut psbt = musig_psbt();
    add_nonce(&mut psbt, key_a(), 1);
    add_partial_sig(&mut psbt, key_b(), 3);
    let stripped = strip_musig_round1(&mut psbt).expect("strip round 1");
    assert!(stripped, "expected the round 1 strip to drop something");
    let input = &psbt.inputs[0].psbt;
    assert!(
        nonces_of(input).is_empty(),
        "expected no 0x1b left, got {:?}",
        nonces_of(input)
    );
    assert_eq!(
        partial_sig_count(input),
        0,
        "expected no 0x1c left after the strip"
    );

    let mut complete = musig_psbt();
    add_nonce(&mut complete, key_a(), 1);
    add_nonce(&mut complete, key_b(), 2);
    let before = complete.clone();
    let stripped = strip_musig_round1(&mut complete).expect("strip round 2");
    assert!(!stripped, "expected nothing dropped once every nonce is in");
    assert_eq!(complete, before, "expected the round 2 PSBT unchanged");

    let mut plain = plain_psbt();
    let stripped = strip_musig_round1(&mut plain).expect("strip plain");
    assert!(
        !stripped,
        "expected nothing dropped from a PSBT with no 0x1a"
    );
}

#[test]
fn done_fingerprints_follow_the_round() {
    let done = |psbt: &PsbtV2| musig_done_fingerprints(psbt).expect("done fingerprints");

    let mut psbt = musig_psbt();
    add_nonce(&mut psbt, key_a(), 1);
    assert_eq!(
        done(&psbt),
        BTreeSet::from([fingerprint_a()]),
        "expected A done in round 1 with A's nonce only"
    );

    add_nonce(&mut psbt, key_b(), 2);
    assert_eq!(
        done(&psbt),
        BTreeSet::new(),
        "expected nobody done in round 2 before any partial signature"
    );

    add_partial_sig(&mut psbt, key_a(), 3);
    assert_eq!(
        done(&psbt),
        BTreeSet::from([fingerprint_a()]),
        "expected A done in round 2 with A's partial signature"
    );

    add_partial_sig(&mut psbt, key_b(), 4);
    assert_eq!(
        done(&psbt),
        BTreeSet::from([fingerprint_a(), fingerprint_b()]),
        "expected A and B done with every partial signature"
    );

    assert_eq!(
        done(&plain_psbt()),
        BTreeSet::new(),
        "expected nobody done on a PSBT with no 0x1a"
    );
}

// ===== Merge =====

#[test]
fn merge_keeps_a_nonce_only_answer() {
    let mut original = musig_psbt();
    add_nonce(&mut original, key_b(), 2);
    let original_bytes = v2_bytes(&original);

    let mut answer = original.clone();
    add_nonce(&mut answer, key_a(), 1);
    let merged = merge_signatures_bytes(&original_bytes, &v0_bytes(&answer))
        .expect("a nonce-only answer merges");
    let merged = PsbtV2::deserialize(&merged).expect("merged psbt parses as v2");
    let expected = BTreeSet::from([(key_a(), 1), (key_b(), 2)]);
    assert_eq!(
        nonces_of(&merged.inputs[0].psbt),
        expected,
        "expected A's and B's nonces in the merge"
    );

    let nothing = merge_signatures_bytes(&original_bytes, &v0_bytes(&original));
    assert!(
        matches!(nothing, Err(Error::NothingSigned)),
        "expected NothingSigned for an answer adding nothing, got {nothing:?}"
    );

    let mut replacing = musig_psbt();
    add_nonce(&mut replacing, key_b(), 9);
    add_nonce(&mut replacing, key_a(), 1);
    let merged =
        merge_signatures_bytes(&original_bytes, &v0_bytes(&replacing)).expect("A's nonce merges");
    let merged = PsbtV2::deserialize(&merged).expect("merged psbt parses as v2");
    assert_eq!(
        nonces_of(&merged.inputs[0].psbt),
        expected,
        "expected B's original nonce kept over the answer's"
    );
}

/// A single-key leaf spend of a wallet whose internal key is `aggregate -> [A, B]`:
/// A's and B's origins, the internal key, and no MuSig2 field at all.
fn leaf_psbt() -> PsbtV2 {
    let mut input = blank_input();
    add_origin(&mut input, key_a(), fingerprint_a());
    add_origin(&mut input, key_b(), fingerprint_b());
    input.psbt.tap_internal_key = Some(aggregate().x_only_public_key().0);
    one_input_psbt(input)
}

#[test]
fn merge_drops_a_nonce_for_an_aggregate_not_listed() {
    let original = leaf_psbt();
    let original_bytes = v2_bytes(&original);
    // A Coldcard told of the internal key's participants answers its leaf signature
    // and a nonce for that key path.
    let mut answer = original.clone();
    answer.inputs[0].psbt.tap_script_sigs.insert(
        (
            key_a().x_only_public_key().0,
            bitcoin::taproot::TapLeafHash::all_zeros(),
        ),
        bitcoin::taproot::Signature::from_slice(&[7; 64]).expect("a 64 byte signature"),
    );
    add_nonce(&mut answer, key_a(), 1);
    let merged = merge_signatures_bytes(&original_bytes, &v0_bytes(&answer))
        .expect("the leaf signature merges");
    let merged = PsbtV2::deserialize(&merged).expect("merged psbt parses as v2");
    assert_eq!(
        merged.inputs[0].psbt.tap_script_sigs.len(),
        1,
        "the leaf signature is kept"
    );
    assert!(
        nonces_of(&merged.inputs[0].psbt).is_empty(),
        "no nonce for an aggregate the original does not list"
    );
}

#[test]
fn internal_participants_only_where_none_are_listed() {
    let mut leaf = leaf_psbt();
    let added = add_internal_participants(&mut leaf, aggregate(), &[key_a(), key_b()])
        .expect("participants added");
    assert_eq!(
        added, 1,
        "the leaf input gains the internal key's participants"
    );
    let listed = musig_input_participants_all(&leaf.inputs[0].psbt).expect("valid 0x1a");
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].aggregate, aggregate());
    assert_eq!(listed[0].participants, vec![key_a(), key_b()]);

    let mut musig = musig_psbt();
    musig.inputs[0].psbt.tap_internal_key = Some(aggregate().x_only_public_key().0);
    let added = add_internal_participants(&mut musig, public_key(0x0d), &[key_a(), key_b()])
        .expect("nothing to add");
    assert_eq!(
        added, 0,
        "an input already listing participants is left as it is"
    );

    let mut stranger = leaf_psbt();
    let added = add_internal_participants(&mut stranger, aggregate(), &[public_key(0x0e)])
        .expect("nothing to add");
    assert_eq!(added, 0, "an input of another wallet is left as it is");
}

// ===== QR =====

/// The PSBT a decoded sign request carries.
fn request_psbt(message: &[u8]) -> Psbt {
    let decoded = bwk_qr_protocol::decode(message).expect("decode request message");
    let Message::Request(request) = decoded else {
        panic!("expected a request, decoded a response");
    };
    let request::Body::Sign(sign) = request.body else {
        panic!("expected a sign request");
    };
    Psbt::deserialize(&sign.psbt).expect("request psbt parses as v0")
}

#[test]
fn qr_round1_request_carries_no_nonce() {
    let mut original = musig_psbt();
    add_nonce(&mut original, key_b(), 2);
    let original_bytes = v2_bytes(&original);

    let mut session = QrSession::new();
    let request = session
        .sign(&["musig descriptor".to_string()], &original_bytes)
        .expect("sign request");
    let wire = request_psbt(request.message());
    let wire_input = &wire.inputs[0];
    assert!(
        nonces_of(wire_input).is_empty(),
        "expected no 0x1b on the wire, got {:?}",
        nonces_of(wire_input)
    );
    assert_eq!(
        partial_sig_count(wire_input),
        0,
        "expected no 0x1c on the wire"
    );

    let mut answer = PsbtV2::from_bitcoin_psbt(wire).expect("wire psbt to v2");
    add_nonce(&mut answer, key_a(), 1);
    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Signed(response::Signed::Psbt(v0_bytes(&answer))),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");
    let merged = match session.decode_response(&encoded) {
        Ok(QrResponse::SignedPsbt(bytes)) => bytes,
        Ok(_) => panic!("expected a SignedPsbt response"),
        Err(e) => panic!("expected the nonce answer accepted, got {e:?}"),
    };
    let merged = PsbtV2::deserialize(&merged).expect("merged psbt parses as v2");
    assert_eq!(
        nonces_of(&merged.inputs[0].psbt),
        BTreeSet::from([(key_a(), 1), (key_b(), 2)]),
        "expected the device's nonce merged next to the one already gathered"
    );
}

#[test]
fn qr_0x0d_is_a_musig_session_error() {
    let mut session = QrSession::new();
    session
        .sign(&["musig descriptor".to_string()], &v2_bytes(&musig_psbt()))
        .expect("sign request");
    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Error(response::ErrorBody {
            message_type: MessageType::Signing,
            error: response::Error::MusigSession,
            message: "no session".to_string(),
        }),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = session
        .decode_response(&encoded)
        .err()
        .expect("a 0x0D answer is refused");
    assert!(
        matches!(error, Error::MusigSession),
        "expected Error::MusigSession, got {error:?}"
    );
    let kind = QrErrorKind::from(&error);
    assert_eq!(
        kind,
        QrErrorKind::MusigSession,
        "expected QrErrorKind::MusigSession, got {kind:?}"
    );
}

// ===== USB =====

/// The v0 PSBT of a `usb_sign_payload` answer.
fn usb_payload_psbt(original: &PsbtV2) -> Psbt {
    let payload = silent::usb_sign_payload(&hex::encode(v2_bytes(original)), "", false);
    assert!(payload.is_ok, "usb payload refused: {}", payload.error);
    let bytes = BASE64_STANDARD
        .decode(&payload.psbt_base64)
        .expect("payload is base64");
    Psbt::deserialize(&bytes).expect("payload parses as v0")
}

#[test]
fn usb_round1_payload_carries_no_nonce() {
    let mut original = musig_psbt();
    add_nonce(&mut original, key_b(), 2);
    add_partial_sig(&mut original, key_a(), 3);

    let payload = usb_payload_psbt(&original);
    let input = &payload.inputs[0];
    assert!(
        nonces_of(input).is_empty(),
        "expected no 0x1b in a round 1 payload, got {:?}",
        nonces_of(input)
    );
    assert_eq!(
        partial_sig_count(input),
        0,
        "expected no 0x1c in a round 1 payload"
    );
}

#[test]
fn usb_round2_payload_keeps_every_nonce() {
    let mut original = musig_psbt();
    add_nonce(&mut original, key_a(), 1);
    add_nonce(&mut original, key_b(), 2);

    let payload = usb_payload_psbt(&original);
    assert_eq!(
        nonces_of(&payload.inputs[0]),
        BTreeSet::from([(key_a(), 1), (key_b(), 2)]),
        "expected both nonces in a round 2 payload"
    );
}

#[test]
fn usb_merge_keeps_the_gathered_nonces() {
    let mut original = musig_psbt();
    add_nonce(&mut original, key_b(), 2);

    let mut answer =
        PsbtV2::from_bitcoin_psbt(usb_payload_psbt(&original)).expect("payload psbt to v2");
    add_nonce(&mut answer, key_a(), 1);
    let signed = BASE64_STANDARD.encode(v0_bytes(&answer));

    let merged = silent::usb_merge_signed(&hex::encode(v2_bytes(&original)), &signed);
    assert!(merged.is_ok, "usb merge refused: {}", merged.error);
    let merged = PsbtV2::deserialize(&hex::decode(&merged.psbt_v2_hex).expect("merged hex"))
        .expect("merged psbt parses as v2");
    assert_eq!(
        nonces_of(&merged.inputs[0].psbt),
        BTreeSet::from([(key_a(), 1), (key_b(), 2)]),
        "expected the device's nonce merged next to the one already gathered"
    );
}
