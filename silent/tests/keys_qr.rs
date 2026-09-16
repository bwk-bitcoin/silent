//! The five signing-flow QR requests, their frame geometry, and every response
//! refusal `QrSession` enforces.
//!
//! None of these cases touch a key record, so no datadir override is needed here.

use std::str::FromStr;

use bitcoin::{absolute::LockTime, bip32::DerivationPath, hashes::Hash, transaction};
use bwk_qr_protocol::{request, response, Message, MessageType, Request, RequestId, Response};
use bwk_sp::bwk_tx::bwk_psbt::{Input as PsbtInput, Output as PsbtOutput, PsbtV2};
use silent::keys::qr::QrSession;

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
    let mut session = QrSession::new().expect("create session");

    let xpubs = session
        .get_xpubs(
            vec![DerivationPath::from_str("m/84'/1'/0'").expect("path")],
            0,
        )
        .expect("get_xpubs request");
    let register = session
        .register_descriptor("wpkh(desc)", 0)
        .expect("register_descriptor request");
    let verify = session
        .verify_address(
            "wpkh(desc)",
            DerivationPath::from_str("m/0/0").expect("path"),
            "address".to_string(),
            0,
        )
        .expect("verify_address request");
    let sign = session
        .sign(&["wpkh(desc)".to_string()], &sign_psbt_bytes(), 0)
        .expect("sign request");
    let sp_descriptor = session
        .get_sp_descriptor(0)
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

// ===== Case 10: a multi-part sign request's frame geometry =====

#[test]
fn test_multi_frame_sign_request_geometry() {
    let mut session = QrSession::new().expect("create session");
    let request = session
        .sign(&["wpkh(desc)".to_string()], &big_sign_psbt_bytes(), 0)
        .expect("sign request");

    assert!(
        request.frame_count() > 1,
        "expected the oversized psbt to split into multiple frames, got {}",
        request.frame_count()
    );
    for index in 0..request.frame_count() {
        let image = request.frame(index).expect("frame within range");
        assert!(
            image.validate_len(),
            "frame {index} has data.len() != width * height ({} != {} * {})",
            image.data.len(),
            image.width,
            image.height
        );
    }

    let out_of_range = request.frame_count();
    assert!(
        request.frame(out_of_range).is_err(),
        "expected frame({out_of_range}) past the end to error"
    );
}

// ===== Case 11: a response answering a different request id is refused =====

#[test]
fn test_request_id_mismatch_is_refused() {
    let mut session = QrSession::new().expect("create session");
    let request = session
        .get_sp_descriptor(0)
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
    let mut session = QrSession::new().expect("create session");
    session
        .get_xpubs(
            vec![
                DerivationPath::from_str("m/84'/1'/0'").expect("path"),
                DerivationPath::from_str("m/84'/1'/1'").expect("path"),
            ],
            0,
        )
        .expect("get_xpubs request");

    let response = Response {
        id: session.outstanding_id().expect("outstanding request"),
        body: response::Body::Xpubs(response::Xpubs {
            xpubs: vec![bwk_qr_protocol::types::Xpub([0u8; 78])],
            fingerprint: bwk_qr_protocol::types::Fingerprint([0u8; 4]),
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
    let mut session = QrSession::new().expect("create session");
    session
        .sign(&["wpkh(desc)".to_string()], &sign_psbt_bytes(), 0)
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
    let mut session = QrSession::new().expect("create session");
    session
        .sign(&["wpkh(desc)".to_string()], &sign_psbt_bytes(), 0)
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
    let mut session = QrSession::new().expect("create session");
    session
        .get_sp_descriptor(0)
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

// ===== Case 16: the encoder's own pixels decode back through a real decoder =====

#[test]
fn test_encoded_frame_decodes_back() {
    let mut session = QrSession::new().expect("create session");
    let request = session
        .get_sp_descriptor(0)
        .expect("get_sp_descriptor request");
    let expected = Request {
        id: request.id(),
        body: request::Body::GetSpDescriptor(request::GetSpDescriptor {}),
    };

    let mut decoder =
        bwk_qr::decoder::Decoder::new(bwk_qr::config::Config::default()).expect("create decoder");
    let image = request.frame(0).expect("single frame request");
    let decoded = decoder.process(image).expect("decode frame");

    assert_eq!(
        decoded,
        vec![bwk_qr::decoder::Decoded::Request(expected)],
        "expected the decoded request to match the one that was built"
    );
}
