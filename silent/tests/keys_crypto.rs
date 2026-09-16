//! Integration tests for the secret envelope: seal/open round trip, wrong password,
//! salt/nonce freshness, the AAD binding and the on-disk format literals.
//!
//! `seal`/`open` are pure functions with no disk access, so this file needs no
//! datadir override.

use base64::prelude::BASE64_STANDARD;
use base64::Engine;
use bitcoin::bip32::Fingerprint;
use bitcoin::Network;
use silent::keys::crypto::{open, seal};
use silent::keys::{Error, Password};

const TEST_MNEMONIC: &str = "abandon abandon abandon abandon abandon abandon abandon abandon \
     abandon abandon abandon about";

fn fingerprint(value: u32) -> Fingerprint {
    Fingerprint::from(value.to_be_bytes())
}

fn password(value: &str) -> Password {
    Password::new(value.to_string())
}

#[test]
fn test_seal_open_round_trip() {
    let fp = fingerprint(1);
    let envelope = seal(
        TEST_MNEMONIC.as_bytes(),
        fp,
        Network::Signet,
        password("correct horse"),
    )
    .expect("seal should succeed");
    let opened = open(&envelope, fp, Network::Signet, password("correct horse"))
        .expect("open should succeed");
    let opened_str = std::str::from_utf8(opened.as_slice()).expect("opened plaintext is utf8");
    assert_eq!(
        opened_str, TEST_MNEMONIC,
        "expected the round tripped mnemonic to match exactly, got {opened_str}"
    );
}

#[test]
fn test_wrong_password_is_refused() {
    let fp = fingerprint(2);
    let envelope = seal(
        TEST_MNEMONIC.as_bytes(),
        fp,
        Network::Signet,
        password("right password"),
    )
    .expect("seal should succeed");
    let result = open(&envelope, fp, Network::Signet, password("wrong password"));
    assert!(
        matches!(result, Err(Error::WrongPassword)),
        "expected WrongPassword, got {result:?}"
    );
    let message = result.unwrap_err().to_string();
    assert!(
        !message.contains(TEST_MNEMONIC),
        "expected the error message not to leak the plaintext, got: {message}"
    );
}

#[test]
fn test_salt_and_nonce_are_fresh() {
    let fp = fingerprint(3);
    let first = seal(
        TEST_MNEMONIC.as_bytes(),
        fp,
        Network::Signet,
        password("same password"),
    )
    .expect("first seal should succeed");
    let second = seal(
        TEST_MNEMONIC.as_bytes(),
        fp,
        Network::Signet,
        password("same password"),
    )
    .expect("second seal should succeed");

    assert_ne!(
        first.salt, second.salt,
        "expected the salt to differ between seals"
    );
    assert_ne!(
        first.nonce, second.nonce,
        "expected the nonce to differ between seals"
    );
    assert_ne!(
        first.ciphertext, second.ciphertext,
        "expected the ciphertext to differ between seals"
    );
}

#[test]
fn test_aad_binds_fingerprint_and_network() {
    let fp_a = fingerprint(4);
    let fp_b = fingerprint(5);
    let envelope = seal(
        TEST_MNEMONIC.as_bytes(),
        fp_a,
        Network::Signet,
        password("password"),
    )
    .expect("seal should succeed");

    let wrong_fingerprint = open(&envelope, fp_b, Network::Signet, password("password"));
    assert!(
        matches!(wrong_fingerprint, Err(Error::WrongPassword)),
        "expected opening under a different fingerprint to be refused, got {wrong_fingerprint:?}"
    );

    let wrong_network = open(&envelope, fp_a, Network::Regtest, password("password"));
    assert!(
        matches!(wrong_network, Err(Error::WrongPassword)),
        "expected opening on a different network to be refused, got {wrong_network:?}"
    );
}

#[test]
fn test_envelope_format_literals() {
    let fp = fingerprint(6);
    let envelope = seal(
        TEST_MNEMONIC.as_bytes(),
        fp,
        Network::Signet,
        password("password"),
    )
    .expect("seal should succeed");

    assert_eq!(
        envelope.kdf, "argon2id",
        "unexpected kdf, got {}",
        envelope.kdf
    );
    assert_eq!(
        envelope.cipher, "xchacha20poly1305",
        "unexpected cipher, got {}",
        envelope.cipher
    );
    assert_eq!(
        envelope.m_cost_kib, 65_536,
        "unexpected m_cost_kib, got {}",
        envelope.m_cost_kib
    );
    assert_eq!(
        envelope.t_cost, 3,
        "unexpected t_cost, got {}",
        envelope.t_cost
    );
    assert_eq!(
        envelope.p_cost, 1,
        "unexpected p_cost, got {}",
        envelope.p_cost
    );
    assert_eq!(
        envelope.version, 1,
        "unexpected version, got {}",
        envelope.version
    );

    let salt = BASE64_STANDARD
        .decode(&envelope.salt)
        .expect("salt should be valid base64");
    assert_eq!(
        salt.len(),
        16,
        "expected a 16 byte salt, got {}",
        salt.len()
    );
    let nonce = BASE64_STANDARD
        .decode(&envelope.nonce)
        .expect("nonce should be valid base64");
    assert_eq!(
        nonce.len(),
        24,
        "expected a 24 byte nonce, got {}",
        nonce.len()
    );

    let json = serde_json::to_value(&envelope).expect("serialize envelope to json");
    let object = json
        .as_object()
        .expect("envelope serializes to a json object");
    for field in [
        "kdf",
        "m_cost_kib",
        "t_cost",
        "p_cost",
        "salt",
        "cipher",
        "nonce",
        "ciphertext",
    ] {
        assert!(
            object.contains_key(field),
            "expected the envelope json to carry the field '{field}', got keys {:?}",
            object.keys().collect::<Vec<_>>()
        );
    }
}

#[test]
fn test_parameters_below_the_minimum_are_refused() {
    let fp = fingerprint(7);
    let mut envelope = seal(
        TEST_MNEMONIC.as_bytes(),
        fp,
        Network::Signet,
        password("password"),
    )
    .expect("seal should succeed");
    let original_m_cost = envelope.m_cost_kib;
    let original_t_cost = envelope.t_cost;

    envelope.m_cost_kib = original_m_cost - 1;
    let weak_memory = open(&envelope, fp, Network::Signet, password("password"));
    assert!(
        matches!(
            weak_memory,
            Err(Error::WeakKdfParams {
                field: "m_cost_kib",
                ..
            })
        ),
        "expected WeakKdfParams for m_cost_kib, got {weak_memory:?}"
    );
    envelope.m_cost_kib = original_m_cost;

    envelope.t_cost = original_t_cost - 1;
    let weak_time = open(&envelope, fp, Network::Signet, password("password"));
    assert!(
        matches!(
            weak_time,
            Err(Error::WeakKdfParams {
                field: "t_cost",
                ..
            })
        ),
        "expected WeakKdfParams for t_cost, got {weak_time:?}"
    );
}
