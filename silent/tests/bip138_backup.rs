// ===== BIP138 descriptor backup tests =====

use std::str::FromStr;

use bip138::{
    ll::{self, crypto::RustBitcoin, Content},
    EncryptedMetadata,
};
use bitcoin::bip32::{DerivationPath, Xpub};
use silent::backup::{encrypt_descriptor, write_descriptor_backup};

const XPUB_A: &str = "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa";
const XPUB_B: &str = "tpubDDzMtUDKwjvTehAmAfwYiBGzUFDcF8AyEaJf78JhQam5knvsGTgqF7VvhuuDZLYityND66FG88sS3q28Yi5Ysumvx5DfMgdm6nc6zntvpHM";
// The unspendable internal key of a Multikey account with no key path: its root is
// the BIP341 NUMS point.
const NUMS_XPUB: &str = "tpubD6NzVbkrYhZ4Wfhvyz5kokDUBLHfzhT59KSamkRVNZhq6EguZVuAf88eJVm6G3ua8YfPChCkyn2CsS7naTCBW1nL4B1Ri2BvhJKMcEvPPeJ";
const LITERAL: &str = "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";

fn root(xpub: &str) -> [u8; 32] {
    Xpub::from_str(xpub)
        .expect("valid xpub")
        .public_key
        .x_only_public_key()
        .0
        .serialize()
}

/// The payload the backup decrypts to with the key `xonly`, None when it does not
/// decrypt.
fn decrypt_with(bytes: &[u8], xonly: [u8; 32]) -> Option<(Content, String)> {
    let (_, secrets, _, nonce, ciphertext) = ll::decode_v1(bytes).expect("a v1 backup");
    let items =
        ll::decrypt_chacha20_poly1305_v1(&RustBitcoin, xonly, &secrets, ciphertext, nonce).ok()?;
    let (content, payload) = items.into_iter().next()?;
    Some((content, String::from_utf8(payload).expect("utf8 payload")))
}

fn key_path_descriptor() -> String {
    format!("tr(musig([73c5da0a/87'/1'/0'/2]{XPUB_A},[b8688df1/87'/1'/0'/2]{XPUB_B})/<0;1>/*)")
}

#[test]
fn each_musig_cosigner_decrypts_the_descriptor() {
    let descriptor = key_path_descriptor();
    let (bytes, excluded) = encrypt_descriptor(&descriptor, true).expect("backup");
    assert!(
        bytes.starts_with(b"BIP138"),
        "a BIP138 backup starts with its magic"
    );
    assert!(excluded.is_empty(), "no key left out, got {excluded:?}");
    for xpub in [XPUB_A, XPUB_B] {
        let decrypted = decrypt_with(&bytes, root(xpub));
        assert_eq!(
            decrypted,
            Some((Content::Bip380, descriptor.clone())),
            "the cosigner {xpub} decrypts the descriptor text"
        );
    }
}

#[test]
fn the_nums_internal_key_is_no_recipient() {
    let descriptor = format!(
        "tr({NUMS_XPUB}/<2147483646;2147483647>/*,pk(musig([73c5da0a/87'/1'/0'/2]{XPUB_A},[b8688df1/87'/1'/0'/2]{XPUB_B})/<0;1>/*))"
    );
    let (bytes, excluded) = encrypt_descriptor(&descriptor, true).expect("backup");
    assert!(
        excluded.is_empty(),
        "the NUMS key is no one's key: nothing to report, got {excluded:?}"
    );
    assert_eq!(
        decrypt_with(&bytes, root(NUMS_XPUB)),
        None,
        "the public NUMS key must not decrypt the backup"
    );
    for xpub in [XPUB_A, XPUB_B] {
        assert!(
            decrypt_with(&bytes, root(xpub)).is_some(),
            "the cosigner {xpub} decrypts the backup"
        );
    }
}

#[test]
fn a_literal_participant_is_left_out_and_reported() {
    // A derivation after musig() takes xpubs only: the literal sits in a musig() whose
    // participants derive on their own.
    let descriptor = format!("tr(musig([73c5da0a/87'/1'/0'/2]{XPUB_A}/0/*,{LITERAL}))");
    let (bytes, excluded) = encrypt_descriptor(&descriptor, true).expect("backup");
    assert_eq!(
        excluded,
        vec![LITERAL.to_string()],
        "the literal key is reported"
    );
    assert!(
        decrypt_with(&bytes, root(XPUB_A)).is_some(),
        "the xpub participant still decrypts"
    );
}

#[test]
fn a_root_exposed_by_a_bare_xpub_is_dropped() {
    let descriptor = format!(
        "tr({NUMS_XPUB}/<2147483646;2147483647>/*,{{pk(musig([73c5da0a/87'/1'/0'/2]{XPUB_A},[b8688df1/87'/1'/0'/2]{XPUB_B})/<0;1>/*),pk([73c5da0a/87'/1'/0'/2]{XPUB_A})}})"
    );
    let (bytes, excluded) = encrypt_descriptor(&descriptor, true).expect("backup");
    assert_eq!(
        decrypt_with(&bytes, root(XPUB_A)),
        None,
        "a root put on chain as is must not decrypt the backup"
    );
    assert!(
        decrypt_with(&bytes, root(XPUB_B)).is_some(),
        "the other cosigner still decrypts"
    );
    assert_eq!(
        excluded.len(),
        2,
        "the bare xpub and the root it exposes are reported, got {excluded:?}"
    );
}

#[test]
fn no_key_to_encrypt_to_is_refused() {
    let descriptor = format!("tr({NUMS_XPUB}/<2147483646;2147483647>/*)");
    let result = encrypt_descriptor(&descriptor, true);
    assert!(
        result.is_err(),
        "a backup no one can decrypt is refused, got {result:?}"
    );
}

#[test]
fn the_privacy_mode_leaves_the_common_paths_out() {
    let descriptor =
        format!("tr(musig([73c5da0a/87'/1'/0']{XPUB_A},[b8688df1/87'/1'/0']{XPUB_B})/<0;1>/*)");
    let common = DerivationPath::from_str("m/87'/1'/0'").expect("valid path");
    for (store, expected) in [(true, vec![common]), (false, Vec::new())] {
        let (bytes, _) = encrypt_descriptor(&descriptor, store).expect("backup");
        let metadata = EncryptedMetadata::from_encrypted_payload(&bytes).expect("metadata");
        assert_eq!(
            metadata.derivation_paths, expected,
            "store_common_paths {store}: the clear derivation paths"
        );
    }
}

#[test]
fn write_descriptor_backup_writes_the_file() {
    let dir = temp_dir::TempDir::new().expect("temp dir");
    let path = dir.path().join("account.bip138");
    let written = write_descriptor_backup(&key_path_descriptor(), &path, true).expect("written");
    assert!(
        written.excluded.is_empty(),
        "no key left out, got {:?}",
        written.excluded
    );
    let bytes = std::fs::read(&path).expect("read back");
    assert!(
        decrypt_with(&bytes, root(XPUB_B)).is_some(),
        "the file decrypts with a cosigner's xpub"
    );
}
