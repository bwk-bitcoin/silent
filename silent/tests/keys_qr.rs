//! The five signing-flow QR requests, their trip through BBQR frames, and every
//! response refusal `QrSession` enforces.
//!
//! None of these cases touch a key record, so no datadir override is needed here.

use std::str::FromStr;

use bitcoin::{
    absolute::LockTime,
    bip32::{ChildNumber, DerivationPath},
    hashes::Hash,
    transaction,
};
use bwk_qr_protocol::{response, Message, MessageType, RequestId, Response};
use bwk_sp::bwk_sign::{
    bwk_descriptor::descriptor::{wpkh, wpkh_path, DescriptorDerivator},
    hot_signer::HotSigner,
};
use bwk_sp::bwk_tx::bwk_psbt::{
    sp::{set_sp_input_ecdh_share, set_sp_input_tweak, sp_input_ecdh_share, sp_input_tweak},
    Input as PsbtInput, Output as PsbtOutput, PsbtV2,
};
use silent::keys::{
    psbt::merge_signatures_bytes,
    qr::{QrResponse, QrSession},
    Error,
};

// ===== PSBT fixtures =====

fn one_input_psbt() -> PsbtV2 {
    PsbtV2 {
        tx_version: transaction::Version::TWO,
        fallback_lock_time: Some(LockTime::ZERO),
        tx_modifiable: None,
        xpub: Default::default(),
        proprietary: Default::default(),
        unknown: Default::default(),
        inputs: vec![PsbtInput {
            previous_output: bitcoin::OutPoint::new(Hash::all_zeros(), 0),
            sequence: bitcoin::Sequence::MAX,
            required_time_lock_time: None,
            required_height_lock_time: None,
            psbt: bitcoin::psbt::Input::default(),
        }],
        outputs: vec![PsbtOutput {
            amount: bitcoin::Amount::from_sat(1_000),
            script_pubkey: Some(bitcoin::ScriptBuf::new()),
            psbt: bitcoin::psbt::Output::default(),
        }],
    }
}

fn sign_psbt_bytes() -> Vec<u8> {
    one_input_psbt().serialize().expect("serialize psbt")
}

/// Comfortably larger than the 1024-byte default BBQR part size, so the request
/// splits into more than one frame.
const BIG_UNKNOWN_LEN: usize = 4_000;
const BIG_UNKNOWN_KEYTYPE: u8 = 0xf0;

fn big_sign_psbt_bytes() -> Vec<u8> {
    let mut psbt = one_input_psbt();
    psbt.unknown.insert(
        bitcoin::psbt::raw::Key {
            type_value: BIG_UNKNOWN_KEYTYPE,
            key: Vec::new(),
        },
        vec![0u8; BIG_UNKNOWN_LEN],
    );
    psbt.serialize().expect("serialize big psbt")
}

fn sp_descriptor_without_origin() -> String {
    let scan_xprv =
        bitcoin::bip32::Xpriv::new_master(bitcoin::Network::Bitcoin, &[0x71; 64]).expect("seed");
    let spend_xprv =
        bitcoin::bip32::Xpriv::new_master(bitcoin::Network::Bitcoin, &[0x72; 64]).expect("seed");
    format!("sp({scan_xprv}/0h,{spend_xprv}/0h)")
}

// ===== Signable PSBT fixtures, for the request-binding and signature-merge cases =====

const TEST_MNEMONIC: &str = "abandon abandon abandon abandon abandon abandon \
    abandon abandon abandon abandon abandon about";
const RECEIVE_INDEX: u32 = 0;

fn blank_input(previous_output: bitcoin::OutPoint) -> PsbtInput {
    PsbtInput {
        previous_output,
        sequence: bitcoin::Sequence::MAX,
        required_time_lock_time: None,
        required_height_lock_time: None,
        psbt: bitcoin::psbt::Input::default(),
    }
}

fn blank_output(script_pubkey: Option<bitcoin::ScriptBuf>) -> PsbtOutput {
    PsbtOutput {
        amount: bitcoin::Amount::from_sat(1_000),
        script_pubkey,
        psbt: bitcoin::psbt::Output::default(),
    }
}

fn public_key(
    secp: &bitcoin::secp256k1::Secp256k1<bitcoin::secp256k1::All>,
    byte: u8,
) -> bitcoin::secp256k1::PublicKey {
    bitcoin::secp256k1::PublicKey::from_secret_key(
        secp,
        &bitcoin::secp256k1::SecretKey::from_slice(&[byte; 32]).expect("valid key"),
    )
}

/// The account-level wpkh() descriptor derived from the test mnemonic, and the origin
/// path it was built at.
fn wpkh_descriptor(network: bitcoin::Network) -> (String, DerivationPath) {
    let account = ChildNumber::from_hardened_idx(0).expect("valid account index");
    let path = wpkh_path(network, account).expect("valid wpkh path");
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    let xpub = signer.xpub(&path);
    (wpkh(xpub).to_string(), path)
}

/// One input spending `descriptor_string` at receive index 0 from the test mnemonic,
/// carrying the bip32_derivation and witness_utxo a hot signer needs to sign it. No
/// silent payment fields.
fn signable_input(
    network: bitcoin::Network,
    descriptor_string: &str,
    path: &DerivationPath,
) -> PsbtInput {
    let descriptor = bwk_sp::bwk_sign::miniscript::Descriptor::<
        bwk_sp::bwk_sign::miniscript::DescriptorPublicKey,
    >::from_str(descriptor_string)
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

    let mut input = blank_input(bitcoin::OutPoint::new(Hash::all_zeros(), 0));
    input
        .psbt
        .bip32_derivation
        .insert(pubkey, (signer.fingerprint(), full_path));
    input.psbt.witness_utxo = Some(bitcoin::TxOut {
        value: bitcoin::Amount::from_sat(100_000),
        script_pubkey: spk,
    });
    input
}

/// A v2 PSBT with one signable, non-SP input. The output script is already derived,
/// so nothing here is pending.
fn non_sp_signable_psbt(
    network: bitcoin::Network,
    descriptor_string: &str,
    path: &DerivationPath,
) -> PsbtV2 {
    let input = signable_input(network, descriptor_string, path);
    PsbtV2 {
        tx_version: transaction::Version::TWO,
        fallback_lock_time: Some(LockTime::ZERO),
        tx_modifiable: None,
        xpub: Default::default(),
        proprietary: Default::default(),
        unknown: Default::default(),
        inputs: vec![input],
        outputs: vec![blank_output(Some(bitcoin::ScriptBuf::new()))],
    }
}

/// The same signable psbt, with an SP input tweak and ECDH share added, so a build
/// of the sign request carries it to the device unconverted (`has_sp_artifacts`).
fn sp_signable_psbt(
    network: bitcoin::Network,
    descriptor_string: &str,
    path: &DerivationPath,
) -> PsbtV2 {
    let mut psbt = non_sp_signable_psbt(network, descriptor_string, path);
    let secp = bitcoin::secp256k1::Secp256k1::new();
    set_sp_input_tweak(&mut psbt.inputs[0].psbt, [0x22; 32]);
    set_sp_input_ecdh_share(
        &mut psbt.inputs[0].psbt,
        public_key(&secp, 0x33),
        public_key(&secp, 0x34),
    );
    psbt
}

/// Only `decode_response` (protocol decode + validation) is exercised here, so an
/// error is always the crate's own `keys::error::Error`, which is `Display`-able
/// but not `Debug`-checked against `Result::expect_err`. Matching by hand keeps
/// every negative case asserting the same way.
fn expect_refused(session: &mut QrSession, bytes: &[u8]) -> String {
    match session.decode_response(bytes) {
        Ok(_) => panic!("expected the response to be refused"),
        Err(e) => e.to_string(),
    }
}

// ===== Case 9: every request round trips through the protocol codec =====

#[test]
fn test_five_requests_round_trip() {
    let mut session = QrSession::new();

    let xpubs = session
        .get_xpubs(vec![DerivationPath::from_str("m/84'/1'/0'").expect("path")])
        .expect("get_xpubs request");
    let register = session
        .register_descriptor("wpkh(desc)")
        .expect("register_descriptor request");
    let verify = session
        .verify_address(
            "wpkh(desc)",
            DerivationPath::from_str("m/0/0").expect("path"),
            "address".to_string(),
        )
        .expect("verify_address request");
    let sign = session
        .sign(&["wpkh(desc)".to_string()], &sign_psbt_bytes())
        .expect("sign request");
    let sp_descriptor = session
        .get_sp_descriptor()
        .expect("get_sp_descriptor request");

    let ids = [
        xpubs.id(),
        register.id(),
        verify.id(),
        sign.id(),
        sp_descriptor.id(),
    ];
    for id in &ids {
        assert_ne!(
            id.0,
            [0, 0],
            "the reserved zero id must never be issued, got {ids:?}"
        );
    }
    let mut sorted_ids: Vec<[u8; 2]> = ids.iter().map(|id| id.0).collect();
    sorted_ids.sort_unstable();
    sorted_ids.dedup();
    assert_eq!(
        sorted_ids.len(),
        ids.len(),
        "expected 5 distinct request ids, got {ids:?}"
    );

    for (request, expected_type) in [
        (&xpubs, MessageType::GetXpubs),
        (&register, MessageType::RegisterDescriptor),
        (&verify, MessageType::AddressVerification),
        (&sign, MessageType::Signing),
        (&sp_descriptor, MessageType::GetSpDescriptor),
    ] {
        match bwk_qr_protocol::decode(request.message()).expect("decode request message") {
            Message::Request(decoded) => {
                assert_eq!(
                    decoded.id,
                    request.id(),
                    "decoded id must match the request's own id"
                );
                assert_eq!(
                    decoded.body.message_type(),
                    expected_type,
                    "decoded body must match the kind that was asked for"
                );
            }
            Message::Response(_) => panic!("expected a request, decoded a response"),
        }
    }
}

// ===== Case 10: a multi-part sign request trips through BBQR frames =====

#[test]
fn test_sign_request_round_trips_through_bbqr_frames() {
    let mut session = QrSession::new();
    let request = session
        .sign(&["wpkh(desc)".to_string()], &big_sign_psbt_bytes())
        .expect("sign request");

    let frames = silent::bbqr::frames(request.message(), 200).expect("frame the request");
    assert!(
        frames.len() > 1,
        "expected the oversized psbt to split into multiple frames, got {}",
        frames.len()
    );
    let mut scanner = silent::bbqr::Scanner::new().expect("create scanner");
    let mut payloads = Vec::new();
    for frame in &frames {
        let rgb: Vec<u8> = frame.data.iter().flat_map(|&luma| [luma; 3]).collect();
        let outcome = scanner.scan(&rgb, frame.width, frame.height);
        payloads.extend(outcome.payloads);
    }
    assert_eq!(
        payloads,
        vec![request.message().to_vec()],
        "the joined payload must be the request message"
    );
}

// ===== Case 11: a response answering a different request id is refused =====

#[test]
fn test_request_id_mismatch_is_refused() {
    let mut session = QrSession::new();
    let request = session
        .get_sp_descriptor()
        .expect("get_sp_descriptor request");
    let outstanding_hex = format!("{:02x}{:02x}", request.id().0[0], request.id().0[1]);

    let wrong_id = RequestId([0xff, 0xff]);
    let response = Response {
        id: wrong_id,
        body: response::Body::SpDescriptor(response::SpDescriptor {
            descriptor: "sp(scan,spend)".to_string(),
        }),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = expect_refused(&mut session, &encoded);
    assert!(
        error.contains(&outstanding_hex) && error.contains("ffff"),
        "expected the error to carry both ids, got {error}"
    );
}

// ===== Case 12: fewer xpubs than paths requested is refused =====

#[test]
fn test_bad_xpub_count_is_refused() {
    let mut session = QrSession::new();
    session
        .get_xpubs(vec![
            DerivationPath::from_str("m/84'/1'/0'").expect("path"),
            DerivationPath::from_str("m/84'/1'/1'").expect("path"),
        ])
        .expect("get_xpubs request");

    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Xpubs(response::Xpubs {
            xpubs: vec![bwk_qr_protocol::types::Xpub([0u8; 78])],
            fingerprint: bwk_qr_protocol::types::Fingerprint([0u8; 4]),
            vendor: "vendor".to_string(),
            model: "model".to_string(),
            version: response::FirmwareVersion {
                major: 1,
                minor: 0,
                patch: 0,
                flag: response::ReleaseFlag::Stable,
            },
            capabilities: response::Capabilities(0),
        }),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = expect_refused(&mut session, &encoded);
    assert!(
        error.contains('2') && error.contains('1'),
        "expected the error to name both counts (2 requested, 1 returned), got {error}"
    );
}

// ===== Case 13: a signed payload that is not a parsable PSBT is refused =====

#[test]
fn test_garbage_signed_psbt_is_refused() {
    let mut session = QrSession::new();
    session
        .sign(&["wpkh(desc)".to_string()], &sign_psbt_bytes())
        .expect("sign request");

    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Signed(response::Signed::Psbt(vec![1, 2, 3])),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = expect_refused(&mut session, &encoded);
    assert!(
        error.to_lowercase().contains("psbt"),
        "expected the error to mention the psbt, got {error}"
    );
}

// ===== Case 14: bare signatures are refused, only a full PSBT is accepted =====

#[test]
fn test_bare_signatures_are_refused() {
    let mut session = QrSession::new();
    session
        .sign(&["wpkh(desc)".to_string()], &sign_psbt_bytes())
        .expect("sign request");

    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Signed(response::Signed::Signatures(Vec::new())),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = expect_refused(&mut session, &encoded);
    assert!(
        error.contains("full") && error.to_lowercase().contains("psbt"),
        "expected the error to say only a full psbt is accepted, got {error}"
    );
}

// ===== Case 15: an sp() descriptor response with no origin is refused =====

#[test]
fn test_origin_less_sp_descriptor_response_is_refused() {
    let mut session = QrSession::new();
    session
        .get_sp_descriptor()
        .expect("get_sp_descriptor request");

    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::SpDescriptor(response::SpDescriptor {
            descriptor: sp_descriptor_without_origin(),
        }),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = expect_refused(&mut session, &encoded);
    assert!(
        error.contains("origin"),
        "expected the error to name the missing origin, got {error}"
    );
}

// ===== Case 17: a v0 signed answer merges into the original v2 request =====

#[test]
fn test_v0_signed_answer_merges_into_the_original_v2() {
    let network = bitcoin::Network::Bitcoin;
    let (descriptor_string, path) = wpkh_descriptor(network);
    let original = non_sp_signable_psbt(network, &descriptor_string, &path);
    let original_bytes = original.serialize().expect("serialize original");

    let mut session = QrSession::new();
    session
        .sign(&[descriptor_string.clone()], &original_bytes)
        .expect("sign request");

    let mut v0 = original.clone().into_bitcoin_psbt().expect("into v0");
    let descriptor = bwk_sp::bwk_sign::miniscript::Descriptor::<
        bwk_sp::bwk_sign::miniscript::DescriptorPublicKey,
    >::from_str(&descriptor_string)
    .expect("valid wpkh descriptor");
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    signer
        .inner_sign(&mut v0, &descriptor)
        .expect("sign succeeds");
    assert!(
        !v0.inputs[0].partial_sigs.is_empty(),
        "expected a partial signature on the signed v0 psbt"
    );
    let signed_v0_bytes = v0.serialize();

    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Signed(response::Signed::Psbt(signed_v0_bytes.clone())),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let decoded = session
        .decode_response(&encoded)
        .expect("response accepted");
    let merged_bytes = match decoded {
        QrResponse::SignedPsbt(bytes) => bytes,
        _ => panic!("expected a SignedPsbt response"),
    };

    let merged_psbt = PsbtV2::deserialize(&merged_bytes).expect("merged psbt parses as v2");
    assert!(
        !merged_psbt.inputs[0].psbt.partial_sigs.is_empty(),
        "expected the merged psbt to carry the device's signature"
    );

    let expected = merge_signatures_bytes(&original_bytes, &signed_v0_bytes)
        .expect("compute the expected merge independently");
    assert_eq!(
        merged_bytes, expected,
        "expected the decoded bytes to equal the original plus the device's signatures"
    );
}

// ===== Case 18: a v2 signed answer keeps the original's SP fields intact =====

#[test]
fn test_v2_signed_answer_keeps_sp_fields_intact() {
    let network = bitcoin::Network::Bitcoin;
    let (descriptor_string, path) = wpkh_descriptor(network);
    let original = sp_signable_psbt(network, &descriptor_string, &path);
    let original_bytes = original.serialize().expect("serialize original");

    let mut session = QrSession::new();
    session
        .sign(&[descriptor_string.clone()], &original_bytes)
        .expect("sign request");

    let mut v0 = original.clone().into_bitcoin_psbt().expect("into v0");
    let descriptor = bwk_sp::bwk_sign::miniscript::Descriptor::<
        bwk_sp::bwk_sign::miniscript::DescriptorPublicKey,
    >::from_str(&descriptor_string)
    .expect("valid wpkh descriptor");
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    signer
        .inner_sign(&mut v0, &descriptor)
        .expect("sign succeeds");

    // The device answers with the full v2 payload it received, signatures added.
    let mut signed_v2 = original.clone();
    signed_v2.inputs[0].psbt.partial_sigs = v0.inputs[0].partial_sigs.clone();
    let signed_v2_bytes = signed_v2.serialize().expect("serialize signed v2");

    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Signed(response::Signed::Psbt(signed_v2_bytes)),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let decoded = session
        .decode_response(&encoded)
        .expect("response accepted");
    let merged_bytes = match decoded {
        QrResponse::SignedPsbt(bytes) => bytes,
        _ => panic!("expected a SignedPsbt response"),
    };

    let merged_psbt = PsbtV2::deserialize(&merged_bytes).expect("merged psbt parses as v2");
    assert!(
        !merged_psbt.inputs[0].psbt.partial_sigs.is_empty(),
        "expected the merged psbt to carry the device's signature"
    );

    let secp = bitcoin::secp256k1::Secp256k1::new();
    assert_eq!(
        sp_input_tweak(&merged_psbt.inputs[0].psbt).expect("valid tweak field"),
        Some([0x22; 32]),
        "expected the sp input tweak to survive the merge"
    );
    assert_eq!(
        sp_input_ecdh_share(&merged_psbt.inputs[0].psbt, public_key(&secp, 0x33))
            .expect("valid ecdh share field"),
        Some(public_key(&secp, 0x34)),
        "expected the sp ecdh share to survive the merge"
    );
}

// ===== Case 19: a signed answer for another transaction is refused, the request stays =====

#[test]
fn test_signed_answer_for_a_different_transaction_is_refused_and_keeps_the_outstanding_request() {
    let network = bitcoin::Network::Bitcoin;
    let (descriptor_string, path) = wpkh_descriptor(network);
    let original = non_sp_signable_psbt(network, &descriptor_string, &path);
    let original_bytes = original.serialize().expect("serialize original");

    let mut session = QrSession::new();
    session
        .sign(&[descriptor_string], &original_bytes)
        .expect("sign request");
    let outstanding_id = session.outstanding_id().expect("outstanding request");

    let other_input = blank_input(bitcoin::OutPoint::new(Hash::from_byte_array([0x44; 32]), 0));
    let other_psbt = PsbtV2 {
        tx_version: transaction::Version::TWO,
        fallback_lock_time: Some(LockTime::ZERO),
        tx_modifiable: None,
        xpub: Default::default(),
        proprietary: Default::default(),
        unknown: Default::default(),
        inputs: vec![other_input],
        outputs: vec![blank_output(Some(bitcoin::ScriptBuf::new()))],
    };
    let other_v0_bytes = other_psbt.into_bitcoin_psbt().expect("into v0").serialize();

    let response = Response {
        id: outstanding_id,
        body: response::Body::Signed(response::Signed::Psbt(other_v0_bytes)),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = match session.decode_response(&encoded) {
        Ok(_) => panic!("a signed psbt for another transaction must be refused"),
        Err(e) => e,
    };
    assert!(
        matches!(error, Error::SignedPsbtMismatch { .. }),
        "expected SignedPsbtMismatch, got {error}"
    );
    assert_eq!(
        session.outstanding_id(),
        Some(outstanding_id),
        "expected the outstanding request to survive a refused response"
    );
}

// ===== Case 20: a signed answer identical to the request signs nothing =====

#[test]
fn test_signed_answer_identical_to_the_request_is_refused_as_nothing_signed() {
    let network = bitcoin::Network::Bitcoin;
    let (descriptor_string, path) = wpkh_descriptor(network);
    let original = non_sp_signable_psbt(network, &descriptor_string, &path);
    let original_bytes = original.serialize().expect("serialize original");

    let mut session = QrSession::new();
    session
        .sign(&[descriptor_string], &original_bytes)
        .expect("sign request");
    let outstanding_id = session.outstanding_id().expect("outstanding request");

    let unsigned_v0_bytes = original
        .clone()
        .into_bitcoin_psbt()
        .expect("into v0")
        .serialize();

    let response = Response {
        id: outstanding_id,
        body: response::Body::Signed(response::Signed::Psbt(unsigned_v0_bytes)),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let error = match session.decode_response(&encoded) {
        Ok(_) => panic!("an unsigned answer must be refused"),
        Err(e) => e,
    };
    assert!(
        matches!(error, Error::NothingSigned),
        "expected NothingSigned, got {error}"
    );
}

// ===== Case 21: an accepted xpubs response carries what it says about the device =====

#[test]
fn test_xpubs_response_carries_the_device() {
    let network = bitcoin::Network::Bitcoin;
    let path = DerivationPath::from_str("m/84'/0'/0'").expect("path");
    let xpub = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC)
        .expect("valid signer")
        .xpub(&path)
        .xkey;

    let mut session = QrSession::new();
    session
        .get_xpubs(vec![path.clone()])
        .expect("get_xpubs request");
    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Xpubs(response::Xpubs {
            xpubs: vec![(&xpub).into()],
            fingerprint: bwk_qr_protocol::types::Fingerprint([0xde, 0xad, 0xbe, 0xef]),
            vendor: "vendor".to_string(),
            model: "model".to_string(),
            version: response::FirmwareVersion {
                major: 1,
                minor: 2,
                patch: 3,
                flag: response::ReleaseFlag::Stable,
            },
            capabilities: response::Capabilities(0x0000_000b),
        }),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let Ok(QrResponse::Xpubs {
        fingerprint,
        xpubs,
        vendor,
        model,
        version,
        capabilities,
    }) = session.decode_response(&encoded)
    else {
        panic!("expected an accepted xpubs response");
    };
    assert_eq!(
        fingerprint.to_string(),
        "deadbeef",
        "unexpected fingerprint"
    );
    assert_eq!(xpubs, vec![(path, xpub)], "unexpected xpubs");
    assert_eq!(vendor, "vendor", "unexpected vendor");
    assert_eq!(model, "model", "unexpected model");
    assert_eq!(version, "1.2.3", "unexpected version");
    assert_eq!(capabilities, 0x0000_000b, "unexpected capabilities");
}

// ===== Case 22: the four account xpubs of an added key come back in request order =====

#[test]
fn test_four_path_xpubs_response_keeps_the_request_order() {
    let network = bitcoin::Network::Bitcoin;
    let signer = HotSigner::new_from_mnemonics(network, TEST_MNEMONIC).expect("valid signer");
    let paths: Vec<DerivationPath> = [
        "m/84'/0'/0'",
        "m/86'/0'/0'",
        "m/48'/0'/0'/2'",
        "m/87'/0'/0'",
    ]
    .iter()
    .map(|path| DerivationPath::from_str(path).expect("path"))
    .collect();
    let expected: Vec<_> = paths
        .iter()
        .map(|path| (path.clone(), signer.xpub(path).xkey))
        .collect();

    let mut session = QrSession::new();
    session.get_xpubs(paths).expect("get_xpubs request");
    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Xpubs(response::Xpubs {
            xpubs: expected.iter().map(|(_, xpub)| xpub.into()).collect(),
            fingerprint: bwk_qr_protocol::types::Fingerprint([0xde, 0xad, 0xbe, 0xef]),
            vendor: "vendor".to_string(),
            model: "model".to_string(),
            version: response::FirmwareVersion {
                major: 1,
                minor: 0,
                patch: 0,
                flag: response::ReleaseFlag::Stable,
            },
            capabilities: response::Capabilities(0),
        }),
    };
    let encoded = bwk_qr_protocol::encode_response(&response).expect("encode response");

    let Ok(QrResponse::Xpubs { xpubs, .. }) = session.decode_response(&encoded) else {
        panic!("expected an accepted xpubs response");
    };
    assert_eq!(
        xpubs, expected,
        "expected the four xpubs paired with their paths in request order"
    );
}
