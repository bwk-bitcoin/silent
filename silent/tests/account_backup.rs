//! Account backups of the built-in accounts: a backup restores the account it was made
//! of, with its address indexes and labels; a malformed backup, a wrong password, an
//! existing account, another kind of account or another SP scan start are refused, leaving
//! nothing behind.

mod common;

use std::str::FromStr;

use bitcoin::{
    bip32::{DerivationPath, Xpriv, Xpub},
    hashes::Hash,
    secp256k1::Secp256k1,
    Txid,
};
use bwk_sp::bwk_sign::hot_signer::HotSigner;
use silent::{
    account_backup::{self, Failure, AAD, MAGIC},
    config_from_file, create_musig_account, create_sp_account, inspect_account_backup,
    keys::crypto::{seal_with_aad, Password},
    new_account_offline, new_musig_account_offline, restore_account_backup, segwit_descriptor_for,
    BackupContents, BackupFailure, LabelKind, LabelSnapshot, MusigCreationRequest, Network,
    SpCreationRequest, MUSIG_PLUGIN_ID,
};

use common::{cleanup_test_account, descriptor_from_mnemonic, test_account_name, TEST_MNEMONIC};

const PASSWORD: &str = "correct horse battery";
const BLINDBIT: &str = "http://127.0.0.1:1";
const ELECTRUM: &str = "tcp://127.0.0.1:2";

/// The master fingerprint of the key from `[seed; 32]` and its xpub at `path`.
fn key(seed: u8, path: &str) -> (String, Xpub) {
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).expect("master key");
    let child = master
        .derive_priv(&secp, &DerivationPath::from_str(path).expect("path"))
        .expect("child key");
    (
        master.fingerprint(&secp).to_string(),
        Xpub::from_priv(&secp, &child),
    )
}

/// The SP key's descriptor, with a segwit sub-account of another key, seed 7.
fn create_sp(name: &str) -> String {
    let (fingerprint, xpub) = key(7, "m/84'/1'/0'");
    let created = create_sp_account(SpCreationRequest {
        account_name: name.into(),
        network: Network::Regtest,
        descriptor: descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        segwit_descriptor: segwit_descriptor_for(
            fingerprint.clone(),
            xpub.to_string(),
            Network::Regtest,
        ),
        blindbit_url: BLINDBIT.into(),
        electrum_url: ELECTRUM.into(),
        ..Default::default()
    });
    assert!(created.is_ok, "create the SP account: {}", created.error);
    fingerprint
}

/// A Multikey account of seeds 1 and 2 under musig(), seed 3 on a leaf.
fn create_multikey(name: &str) -> Vec<String> {
    let role = |seed| {
        let (fingerprint, xpub) = key(seed, "m/87'/1'/0'/2");
        (
            fingerprint.clone(),
            format!("[{fingerprint}/87'/1'/0'/2]{xpub}"),
        )
    };
    let (a, b, c) = (role(1), role(2), role(3));
    let created = create_musig_account(MusigCreationRequest {
        account_name: name.into(),
        network: Network::Regtest,
        descriptor: format!("tr(musig({},{})/<0;1>/*,pk({}/<2;3>/*))", a.1, b.1, c.1),
        electrum_url: ELECTRUM.into(),
        verify_certificate: true,
    });
    assert!(
        created.is_ok,
        "create the Multikey account: {}",
        created.error
    );
    let mut fingerprints = vec![a.0, b.0, c.0];
    fingerprints.sort();
    fingerprints
}

fn labels_of(snapshot: LabelSnapshot) -> Vec<(u8, String, String)> {
    assert!(snapshot.is_ok, "label snapshot: {}", snapshot.error);
    let mut labels: Vec<_> = snapshot
        .labels
        .into_iter()
        .map(|label| (label.kind.repr, label.id, label.text))
        .collect();
    labels.sort();
    labels
}

/// What an SP account holds that a backup carries: its segwit addresses handed out and
/// its labels.
struct SpState {
    addresses: Vec<String>,
    labels: Vec<(u8, String, String)>,
}

fn sp_state(name: &str) -> SpState {
    let config = config_from_file(name.into());
    assert!(config.is_ok(), "read the SP config: {}", config.get_error());
    let mut account = new_account_offline(&config);
    assert!(
        account.is_ok(),
        "open the SP account: {}",
        account.get_error()
    );
    let addresses = account
        .receive_addresses()
        .into_iter()
        .filter(|address| address.kind == "segwit")
        .map(|address| address.address)
        .collect();
    let labels = labels_of(account.labels());
    assert!(
        account.shutdown_backend().is_empty(),
        "close the SP account"
    );
    SpState { addresses, labels }
}

fn assert_refused(contents: &BackupContents, failure: BackupFailure, what: &str) {
    assert!(!contents.is_ok, "{what}: the backup is refused");
    assert_eq!(contents.failure, failure, "{what}: {}", contents.error);
    assert!(!contents.error.is_empty(), "{what}: the refusal says why");
}

/// Backs the SP account `name` up after handing out two segwit addresses and labelling
/// one of them and a transaction.
fn sp_backup(name: &str) -> Vec<u8> {
    let config = config_from_file(name.into());
    let mut account = new_account_offline(&config);
    assert!(
        account.is_ok(),
        "open the SP account: {}",
        account.get_error()
    );
    let first = account.new_receive_address("segwit");
    let second = account.new_receive_address("segwit");
    assert!(first.is_ok && second.is_ok, "hand out two segwit addresses");
    let txid = Txid::from_byte_array([9; 32]).to_string();
    for (kind, id, text) in [
        (LabelKind::Address, first.address.address.clone(), "rent"),
        (LabelKind::Transaction, txid, "salary"),
    ] {
        let saved = account.set_label(kind, id, text.into());
        assert!(saved.is_ok, "label {text}: {}", saved.error);
    }
    let backup = account.backup(PASSWORD.into());
    assert!(backup.is_ok, "back the SP account up: {}", backup.error);
    assert!(
        account.shutdown_backend().is_empty(),
        "close the SP account"
    );
    backup.bytes.into_iter().collect()
}

#[test]
fn an_sp_backup_restores_its_indexes_and_labels() {
    let name = test_account_name();
    let key_fingerprint = create_sp(&name);
    let bytes = sp_backup(&name);
    let before = sp_state(&name);
    assert_eq!(before.addresses.len(), 2, "two segwit addresses handed out");

    let contents = inspect_account_backup(&bytes, PASSWORD.into());
    assert!(contents.is_ok, "inspect the backup: {}", contents.error);
    assert_eq!(
        (
            contents.plugin_id.as_str(),
            contents.account.as_str(),
            contents.network,
            contents.scan_start,
            contents.labels,
            contents.saved_psbts_excluded,
        ),
        ("sp", name.as_str(), Network::Regtest, 1, 2, false),
        "an SP backup's account, scan start and labels"
    );
    assert_eq!(
        contents.endpoints,
        vec![BLINDBIT.to_string(), ELECTRUM.to_string()],
        "both servers"
    );
    let tips: Vec<_> = contents
        .tips
        .iter()
        .map(|tip| (tip.kind.as_str(), tip.receive))
        .collect();
    assert_eq!(tips, vec![("segwit", 2)], "the segwit index handed out");
    let sp_key = HotSigner::new_from_mnemonics(bitcoin::Network::Regtest, TEST_MNEMONIC)
        .expect("test signer")
        .fingerprint()
        .to_string();
    let mut signers = vec![key_fingerprint, sp_key];
    signers.sort();
    assert_eq!(
        contents.signers, signers,
        "the SP key and the segwit key sign"
    );

    cleanup_test_account(&name);
    let restored = restore_account_backup("sp", &bytes, PASSWORD.into());
    assert!(restored.is_ok, "restore the SP account: {}", restored.error);
    assert_eq!(restored.account, name, "restored under its own name");
    let after = sp_state(&name);
    assert_eq!(
        after.addresses, before.addresses,
        "the restored account handed out the same segwit addresses"
    );
    assert_eq!(after.labels, before.labels, "the labels came back");
    cleanup_test_account(&name);
}

#[test]
fn a_multikey_backup_restores_its_indexes_and_labels_without_saved_psbts() {
    let name = test_account_name();
    let signers = create_multikey(&name);
    let config = config_from_file(name.clone());
    let mut account = new_musig_account_offline(&config);
    assert!(
        account.is_ok(),
        "open the Multikey account: {}",
        account.get_error()
    );
    let handed = account.new_receive_address("taproot");
    assert!(handed.is_ok, "hand out a taproot address: {}", handed.error);
    let saved = account.set_label(
        LabelKind::Address,
        handed.address.address.clone(),
        "invoice".into(),
    );
    assert!(saved.is_ok, "label the address: {}", saved.error);
    let addresses = account.receive_addresses();
    let labels = labels_of(account.labels());
    assert!(
        !account.backup(String::new()).is_ok,
        "no backup is sealed under an empty password"
    );
    let backup = account.backup(PASSWORD.into());
    assert!(
        backup.is_ok,
        "back the Multikey account up: {}",
        backup.error
    );
    assert!(account.shutdown_backend().is_empty(), "close the account");
    let bytes: Vec<u8> = backup.bytes.into_iter().collect();

    let contents = inspect_account_backup(&bytes, PASSWORD.into());
    assert!(contents.is_ok, "inspect the backup: {}", contents.error);
    assert_eq!(
        (
            contents.plugin_id.as_str(),
            contents.scan_start,
            contents.labels,
            contents.saved_psbts_excluded,
        ),
        (MUSIG_PLUGIN_ID, 0, 1, true),
        "a Multikey backup scans nothing from a block and leaves its saved PSBTs out"
    );
    assert_eq!(contents.signers, signers, "every key of the policy");
    assert_eq!(contents.endpoints, vec![ELECTRUM.to_string()], "its server");

    cleanup_test_account(&name);
    let restored = restore_account_backup(MUSIG_PLUGIN_ID, &bytes, PASSWORD.into());
    assert!(
        restored.is_ok,
        "restore the Multikey account: {}",
        restored.error
    );
    let config = config_from_file(name.clone());
    let mut account = new_musig_account_offline(&config);
    assert!(
        account.is_ok(),
        "open the restored account: {}",
        account.get_error()
    );
    let restored_addresses: Vec<_> = account
        .receive_addresses()
        .into_iter()
        .map(|address| address.address)
        .collect();
    let original: Vec<_> = addresses
        .into_iter()
        .map(|address| address.address)
        .collect();
    assert_eq!(
        restored_addresses, original,
        "the same addresses handed out"
    );
    assert_eq!(labels_of(account.labels()), labels, "the labels came back");
    assert!(
        account.shutdown_backend().is_empty(),
        "close the restored account"
    );
    cleanup_test_account(&name);
}

#[test]
fn malformed_backups_and_wrong_passwords_are_refused() {
    let name = test_account_name();
    create_sp(&name);
    let bytes = sp_backup(&name);
    cleanup_test_account(&name);

    let wrong = inspect_account_backup(&bytes, "another password".into());
    assert_refused(&wrong, BackupFailure::WrongPassword, "a wrong password");
    let restored = restore_account_backup("sp", &bytes, "another password".into());
    assert_eq!(
        (restored.is_ok, restored.failure),
        (false, BackupFailure::WrongPassword),
        "a wrong password restores nothing: {}",
        restored.error
    );
    assert!(
        !config_from_file(name.clone()).is_ok(),
        "no account is left by a refused restore"
    );

    // A damaged ciphertext fails its authentication as a wrong password does: the two
    // cannot be told apart, and neither opens.
    let text = String::from_utf8(bytes.clone()).expect("a backup is text");
    let at = text.find("\"ciphertext\":\"").expect("a ciphertext") + 14;
    let mut damaged = bytes.clone();
    damaged[at] = if damaged[at] == b'A' { b'B' } else { b'A' };
    assert_refused(
        &inspect_account_backup(&damaged, PASSWORD.into()),
        BackupFailure::WrongPassword,
        "a damaged ciphertext",
    );
    for (what, malformed) in [
        ("random bytes", b"not a backup at all".to_vec()),
        ("a cut backup", bytes[..bytes.len() / 2].to_vec()),
    ] {
        assert_refused(
            &inspect_account_backup(&malformed, PASSWORD.into()),
            BackupFailure::Malformed,
            what,
        );
        let restored = restore_account_backup("sp", &malformed, PASSWORD.into());
        assert_eq!(
            (restored.is_ok, restored.failure),
            (false, BackupFailure::Malformed),
            "{what} restores nothing: {}",
            restored.error
        );
    }
    assert!(
        !config_from_file(name.clone()).is_ok(),
        "no account is left by a malformed backup"
    );
}

#[test]
fn a_restore_never_replaces_an_account_or_takes_another_kind() {
    let name = test_account_name();
    create_sp(&name);
    let bytes = sp_backup(&name);
    let before = sp_state(&name);

    let restored = restore_account_backup("sp", &bytes, PASSWORD.into());
    assert_eq!(
        (restored.is_ok, restored.failure),
        (false, BackupFailure::Exists),
        "an existing account is not restored over: {}",
        restored.error
    );
    let kept = sp_state(&name);
    assert_eq!(
        (kept.addresses, kept.labels),
        (before.addresses, before.labels),
        "the existing account is untouched"
    );

    cleanup_test_account(&name);
    let restored = restore_account_backup(MUSIG_PLUGIN_ID, &bytes, PASSWORD.into());
    assert_eq!(
        (restored.is_ok, restored.failure),
        (false, BackupFailure::Unsupported),
        "an SP backup is not restored as a Multikey account: {}",
        restored.error
    );
    assert!(
        !config_from_file(name.clone()).is_ok(),
        "nothing is created for another kind of account"
    );
}

/// A backup holding `payload` as it is, sealed as the app seals one.
fn sealed(payload: serde_json::Value) -> Vec<u8> {
    let envelope = seal_with_aad(
        &serde_json::to_vec(&payload).expect("json"),
        AAD,
        Password::new(PASSWORD.into()),
    )
    .expect("seal");
    let mut bytes = MAGIC.to_vec();
    bytes.extend(serde_json::to_vec(&envelope).expect("envelope"));
    bytes
}

/// The JSON of the saved config of the account `name`, which is then removed.
fn removed_config(name: &str) -> serde_json::Value {
    let config = config_from_file(name.into());
    assert!(config.is_ok(), "read the config: {}", config.get_error());
    let json = serde_json::to_value(&*config).expect("config json");
    cleanup_test_account(name);
    json
}

#[test]
fn a_backup_of_a_newer_version_is_refused() {
    let name = test_account_name();
    create_multikey(&name);
    let config = removed_config(&name);
    let bytes = sealed(serde_json::json!({ "version": 99, "config": config }));
    assert!(
        matches!(
            account_backup::open(&bytes, PASSWORD.into()),
            Err(Failure::Unsupported(_))
        ),
        "a newer backup is refused, not read as this version's"
    );
    assert_refused(
        &inspect_account_backup(&bytes, PASSWORD.into()),
        BackupFailure::Unsupported,
        "a newer backup",
    );
}

#[test]
fn a_restore_failing_after_creation_leaves_no_account() {
    let name = test_account_name();
    create_multikey(&name);
    let config = removed_config(&name);
    let bytes = sealed(serde_json::json!({
        "version": account_backup::VERSION,
        "config": config,
        "scan_start": null,
        "tips": [],
        "labels": [{ "kind": "unknown", "id": "x", "text": "y" }],
    }));
    let restored = restore_account_backup(MUSIG_PLUGIN_ID, &bytes, PASSWORD.into());
    assert_eq!(
        (restored.is_ok, restored.failure),
        (false, BackupFailure::Other),
        "the restore fails once the account is created: {}",
        restored.error
    );
    assert!(
        !config_from_file(name).is_ok(),
        "the account it created is removed"
    );
}

#[test]
fn an_sp_backup_scanning_from_another_block_is_refused() {
    let name = test_account_name();
    create_sp(&name);
    let config = removed_config(&name);
    let backup = |scan_start: u32| {
        sealed(serde_json::json!({
            "version": account_backup::VERSION,
            "config": config,
            "scan_start": scan_start,
            "tips": [],
            "labels": [],
        }))
    };

    let refused = restore_account_backup("sp", &backup(500), PASSWORD.into());
    assert_eq!(
        (refused.is_ok, refused.failure),
        (false, BackupFailure::Unsupported),
        "a backup scanning from another block is refused: {}",
        refused.error
    );
    assert!(
        refused.error.contains("scans from block Some(500)"),
        "the refusal names the block: {}",
        refused.error
    );
    assert!(
        !config_from_file(name.clone()).is_ok(),
        "nothing is created for a refused scan start"
    );

    let restored = restore_account_backup("sp", &backup(1), PASSWORD.into());
    assert!(
        restored.is_ok,
        "the block every new account scans from restores: {}",
        restored.error
    );
    assert!(
        config_from_file(name.clone()).is_ok(),
        "the restored account is saved"
    );
    cleanup_test_account(&name);
}
