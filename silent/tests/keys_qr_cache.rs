//! The cached MuSig QR exchange through the module: the material step persists the
//! record, the request is issued, an emulated peer answers with real session nonces,
//! and the decoded answer lands in the cache as a prepared session. These are the
//! exact module functions the GUI engine's worker and slots call, in the same order.

use std::str::FromStr;
use std::sync::OnceLock;

use bitcoin::bip32::{DerivationPath, Fingerprint};
use bitcoin::hashes::Hash;
use bwk_qr_protocol::{decode, encode_response, request, response, Message, Response};
use bwk_sp::bwk_sign::{
    bwk_descriptor::{
        descriptor::Descriptor,
        wallet::{WalletDescriptor, WalletDescriptorExt},
    },
    hot_musig::HotMusigSession,
    hot_signer::HotSigner,
    musig::{expression_slots, MusigSessionId},
};
use silent::{KeyRouteKind, Network as FfiNetwork};

// ===== Datadir =====

static DATADIR: OnceLock<temp_dir::TempDir> = OnceLock::new();

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

const MNEMONIC_A: &str =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
const MNEMONIC_B: &str =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";
// A second pair, so a case's cache never mixes with another case's fingerprint.
const MNEMONIC_C: &str =
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above";
const MNEMONIC_D: &str = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong";

fn signers() -> (HotSigner, HotSigner) {
    signers_on(MNEMONIC_A, MNEMONIC_B)
}

fn signers_on(a: &str, b: &str) -> (HotSigner, HotSigner) {
    (
        HotSigner::new_from_mnemonics(bitcoin::Network::Regtest, a).expect("signer a"),
        HotSigner::new_from_mnemonics(bitcoin::Network::Regtest, b).expect("signer b"),
    )
}

fn policy_text(a: &HotSigner, b: &HotSigner) -> String {
    let path = DerivationPath::from_str("m/87'/1'/0'").expect("account path");
    format!(
        "tr(musig([{}/87h/1h/0h]{}/<0;1>/*,[{}/87h/1h/0h]{}/<0;1>/*))",
        a.fingerprint(),
        a.xpub(&path).xkey,
        b.fingerprint(),
        b.xpub(&path).xkey
    )
}

fn wallet(policy: &str) -> WalletDescriptor {
    Descriptor::from_str(policy)
        .expect("valid musig policy")
        .into_miniscript()
        .expect("a miniscript policy")
}

/// The prepared answer a faithful peer sends for the asked coverage, with the device
/// session's real nonces on every owned slot.
fn prepared_answer(
    session: &HotMusigSession,
    policy: &str,
    session_id: [u8; 32],
    max_inputs: u8,
    derivations: &[request::MusigDerivation],
    fingerprint: Fingerprint,
) -> response::MusigPrepared {
    let parsed = wallet(policy);
    let mut nonces = Vec::new();
    for derivation in derivations {
        let definite = parsed.single_paths().expect("paths")[derivation.keychain as usize]
            .derive_at(derivation.index)
            .expect("derive");
        let advertised = session
            .public_nonces(&parsed, derivation.keychain as usize, derivation.index)
            .expect("device nonces");
        for input in 0..u32::from(max_inputs) {
            for info in expression_slots(input as usize, &definite).expect("expression slots") {
                if info.signer.0 != fingerprint {
                    continue;
                }
                let entry = advertised
                    .iter()
                    .find(|entry| {
                        entry.input_index as u32 == input
                            && entry.expression_index as u32 == info.expression_index as u32
                    })
                    .expect("device nonce for the slot");
                nonces.push(response::MusigNonce {
                    derivation: *derivation,
                    input_index: input,
                    expression_index: info.expression_index as u32,
                    participant: bwk_qr_protocol::types::PublicKey(
                        entry.nonce.participant.serialize(),
                    ),
                    aggregate: bwk_qr_protocol::types::PublicKey(info.slot.aggregate.serialize()),
                    leaf_hash: info.slot.leaf_hash.map(|hash| hash.to_byte_array()),
                    nonce: entry.nonce.nonce,
                });
            }
        }
    }
    response::MusigPrepared {
        session_id,
        max_inputs,
        nonces,
    }
}

// ===== The exchange =====

#[test]
fn cached_prepare_exchange_issues_the_request_and_delivers_the_answer() {
    let dir = datadir();
    assert!(dir.exists(), "the datadir fixture is set");
    let (a, _b) = signers();
    let policy = policy_text(&a, &_b);
    let fingerprint = a.fingerprint().to_string();
    let mut module = silent::new_keys_module(FfiNetwork::Regtest);

    // The worker step: the record is persisted, the material handed back.
    let material = silent::qr_musig_prepare_material(&fingerprint, &policy);
    // The slot step: the wire request is built and made the outstanding one.
    let message = module.qr_request_prepare_musig(material);
    assert!(
        message.is_ok,
        "the prepare request is issued: {}",
        message.error
    );
    assert!(
        !message.payload.is_empty(),
        "the wire bytes are handed over"
    );

    // The emulated peer: it answers the request it was shown, its own session nonces
    // on the asked coverage.
    let request = match decode(&message.payload).expect("decode the wire request") {
        Message::Request(request) => request,
        Message::Response(_) => panic!("expected a request, decoded a response"),
    };
    let request::Body::PrepareMusig(preparation) = &request.body else {
        panic!("expected a PrepareMusig request");
    };
    let wallet = wallet(&policy);
    let device = a
        .create_musig_session(
            &wallet,
            MusigSessionId(preparation.session_id),
            preparation.max_inputs,
        )
        .expect("device session");
    let answer = prepared_answer(
        &device,
        &policy,
        preparation.session_id,
        preparation.max_inputs,
        &preparation.derivations,
        a.fingerprint(),
    );
    let encoded = encode_response(&Response {
        id: request.id,
        body: response::Body::MusigPrepared(answer),
    })
    .expect("encode the peer's answer");

    // The engine decodes against the outstanding request and hands the result to the
    // cache: the session lands prepared.
    let decoded = module.qr_decode_response(&encoded);
    assert!(
        decoded.is_ok(),
        "the peer's answer decodes: {}",
        decoded.get_error()
    );
    assert_eq!(
        decoded.kind(),
        silent::QrResponseKind::MusigPrepared,
        "a prepared answer is recognized as its own kind"
    );
    let accepted = silent::qr_musig_accept(decoded, &fingerprint, &policy, "");
    assert!(accepted.is_ok, "the answer is accepted: {}", accepted.error);
    assert_eq!(
        silent::musig_session_count(&fingerprint, &policy, KeyRouteKind::Qr),
        1,
        "the cache holds exactly the prepared session"
    );
}

#[test]
fn an_answer_for_another_session_is_refused_before_the_cache() {
    let dir = datadir();
    assert!(dir.exists(), "the datadir fixture is set");
    let (a, _b) = signers_on(MNEMONIC_C, MNEMONIC_D);
    let policy = policy_text(&a, &_b);
    let fingerprint = a.fingerprint().to_string();
    let mut module = silent::new_keys_module(FfiNetwork::Regtest);

    let material = silent::qr_musig_prepare_material(&fingerprint, &policy);
    let message = module.qr_request_prepare_musig(material);
    assert!(message.is_ok, "the prepare request is issued");

    let request = match decode(&message.payload).expect("decode the wire request") {
        Message::Request(request) => request,
        Message::Response(_) => panic!("expected a request, decoded a response"),
    };
    let request::Body::PrepareMusig(preparation) = &request.body else {
        panic!("expected a PrepareMusig request");
    };
    let wallet = wallet(&policy);
    let device = a
        .create_musig_session(
            &wallet,
            MusigSessionId(preparation.session_id),
            preparation.max_inputs,
        )
        .expect("device session");
    let answer = prepared_answer(
        &device,
        &policy,
        preparation.session_id,
        preparation.max_inputs,
        &preparation.derivations,
        a.fingerprint(),
    );
    let mut stranger = answer.clone();
    stranger.session_id = [0xab; 32];
    let encoded = encode_response(&Response {
        id: request.id,
        body: response::Body::MusigPrepared(stranger),
    })
    .expect("encode the stranger's answer");

    let decoded = module.qr_decode_response(&encoded);
    assert!(
        !decoded.is_ok(),
        "an answer for another session is refused by the codec"
    );

    // The refusal consumed nothing: the right answer still prepares the session.
    let encoded = encode_response(&Response {
        id: request.id,
        body: response::Body::MusigPrepared(answer),
    })
    .expect("encode the honest answer");
    let decoded = module.qr_decode_response(&encoded);
    assert!(
        decoded.is_ok(),
        "the exchange stays open after a refusal: {}",
        decoded.get_error()
    );
    let accepted = silent::qr_musig_accept(decoded, &fingerprint, &policy, "");
    assert!(
        accepted.is_ok,
        "the honest answer is accepted: {}",
        accepted.error
    );
    assert_eq!(
        silent::musig_session_count(&fingerprint, &policy, KeyRouteKind::Qr),
        1,
        "the cache holds exactly the prepared session"
    );
}
