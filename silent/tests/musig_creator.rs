//! What the musig creator needs before an account exists: the receive address a
//! compiled descriptor hands out first, and how it is checked on a device.

mod common;

use std::str::FromStr;
use std::sync::OnceLock;

use bitcoin::{
    bip32::{DerivationPath, Xpriv, Xpub},
    secp256k1::Secp256k1,
};
use bwk_sp::receiver::bip39::Mnemonic;
use silent::{
    compile_policy, musig_first_receive_address, new_config, new_musig_account, review_policy,
    set_datadir, Network, PolicyKey, PolicyKeyType, PolicyLocktime, PolicyPath, PolicySemantic,
    PolicySpec, MUSIG_PLUGIN_ID,
};
use temp_dir::TempDir;

use common::{cleanup_test_account, test_account_name, TEST_MNEMONIC};

const ACCOUNT_PATH: &str = "m/87'/1'/0'";

const SECOND_MNEMONIC: &str =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";
const THIRD_MNEMONIC: &str =
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above";

// `set_datadir` is process wide: every test of this binary shares one TempDir.
static DATADIR_GUARD: OnceLock<TempDir> = OnceLock::new();

// ===== Test Helpers =====

fn shared_datadir() {
    let tmp = DATADIR_GUARD.get_or_init(|| TempDir::new().expect("create temp dir"));
    set_datadir(tmp.path().to_path_buf());
}

/// The key of `mnemonic` with its account xpub at `m/87'/1'/0'` (Regtest).
fn key(mnemonic: &str, key_type: PolicyKeyType) -> PolicyKey {
    let secp = Secp256k1::new();
    let seed = Mnemonic::parse(mnemonic)
        .expect("valid test mnemonic")
        .to_seed("");
    let master = Xpriv::new_master(bitcoin::Network::Regtest, &seed).expect("valid seed");
    let account = master
        .derive_priv(
            &secp,
            &DerivationPath::from_str(ACCOUNT_PATH).expect("valid path"),
        )
        .expect("derivable path");
    PolicyKey {
        fingerprint: master.fingerprint(&secp).to_string(),
        account_path: ACCOUNT_PATH.into(),
        account_xpub: Xpub::from_priv(&secp, &account).to_string(),
        key_type,
        mandatory: false,
    }
}

/// One 2-of-3 path over the three test mnemonics, no recovery path.
fn two_of_three_spec() -> PolicySpec {
    PolicySpec {
        network: Network::Regtest,
        paths: vec![PolicyPath {
            semantic: PolicySemantic::Multi,
            threshold: 2,
            locktime: PolicyLocktime::None,
            locktime_value: 0,
            keys: vec![
                key(TEST_MNEMONIC, PolicyKeyType::Internal),
                key(SECOND_MNEMONIC, PolicyKeyType::External),
                key(THIRD_MNEMONIC, PolicyKeyType::Cosigner),
            ],
        }],
    }
}

// ===== Receive Address Tests =====

#[test]
fn musig_first_receive_address_matches_the_account() {
    shared_datadir();
    let spec = two_of_three_spec();

    let compiled = compile_policy(spec.clone());
    assert!(
        compiled.is_ok,
        "compile_policy should succeed, got error {:?}",
        compiled.error
    );

    let review = review_policy(compiled.descriptor.clone());
    assert!(
        review.is_ok,
        "review_policy should succeed, got error {:?}",
        review.error
    );
    assert_eq!(
        review.paths.len(),
        1,
        "review should give one path, got {}",
        review.paths.len()
    );
    assert_eq!(
        review.leaf_count, 3,
        "a 2-of-3 is three leaves, got {}",
        review.leaf_count
    );
    let key_types: Vec<String> = review.paths[0]
        .keys
        .iter()
        .map(|k| format!("{:?}", k.key_type))
        .collect();
    assert_eq!(
        key_types,
        vec!["Internal", "External", "Cosigner"],
        "review key types in path order"
    );

    let receive = musig_first_receive_address(compiled.descriptor.clone(), Network::Regtest);
    assert!(
        receive.is_ok,
        "musig_first_receive_address should succeed, got error {:?}",
        receive.error
    );
    assert!(
        receive.address.starts_with("bcrt1p"),
        "address should be regtest taproot, got {}",
        receive.address
    );
    assert_eq!(
        receive.path, "0/1",
        "a new account hands out receive index 1 first"
    );

    let account_name = test_account_name();
    let config = new_config(
        account_name.clone(),
        Network::Regtest,
        compiled.descriptor.clone(),
        String::new(),
        String::new(),
        546,
        MUSIG_PLUGIN_ID.to_string(),
    );
    config.to_file();
    let mut account = new_musig_account(account_name.clone());
    assert!(
        account.is_ok(),
        "account should open: {}",
        account.get_error()
    );

    let first = account.new_addr();
    assert_eq!(
        first, receive.address,
        "the account's first address should be the one shown before install"
    );
    let info = account.address_verify_info(&first);
    assert_eq!(
        info.descriptor, receive.descriptor,
        "verify descriptor should match"
    );
    assert_eq!(info.path, receive.path, "verify path should match");

    drop(account);
    cleanup_test_account(&account_name);
}

#[test]
fn musig_first_receive_address_refuses_a_bad_descriptor() {
    let receive = musig_first_receive_address("tr(not a key)".to_string(), Network::Regtest);
    assert!(!receive.is_ok, "a bad descriptor should fail");
    assert!(
        !receive.error.is_empty(),
        "a bad descriptor should carry an error"
    );
    assert!(
        receive.address.is_empty(),
        "a bad descriptor gives no address, got {}",
        receive.address
    );
}
