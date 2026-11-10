//! Account backups of the built-in accounts: a backup is a BIP139 document encrypted with
//! BIP138 to the account's keys, and restores the account it was made of, with its
//! address indexes and labels. A restore reads BIP138(BIP139), BIP138(descriptor), a
//! plain BIP139 document and a plain descriptor; anything else, a key that does not open
//! the backup, an existing account or another kind of account is refused, leaving nothing
//! behind.

mod common;

use std::str::FromStr;

use bip138::Content;
use bitcoin::{
    bip32::{ChildNumber, DerivationPath, Xpriv, Xpub},
    hashes::Hash,
    secp256k1::Secp256k1,
    Txid,
};
use bwk_sp::{
    bwk_sign::{bwk_descriptor::sp_descriptor::SpDescriptor, hot_signer::HotSigner},
    receiver::{scan_path, spend_path},
};
use silent::{
    app_set_last_endpoint,
    backup::{encrypt_descriptor, Encrypted},
    bip139::WalletBackup,
    config_from_file, create_musig_account, create_sp_account, inspect_account_backup,
    new_account_offline, new_musig_account_offline, restore_account_backup, segwit_descriptor_for,
    BackendServer, BackupContents, BackupFailure, BackupRestore, LabelKind, LabelSnapshot,
    MusigCreationRequest, Network, SpCreationRequest, MUSIG_PLUGIN_ID,
};

use common::{cleanup_test_account, descriptor_from_mnemonic, test_account_name, TEST_MNEMONIC};

const BLINDBIT: &str = "http://127.0.0.1:1";
const ELECTRUM: &str = "tcp://127.0.0.1:2";
/// A valid mnemonic of a key no test account uses.
const OTHER_MNEMONIC: &str =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";
const SPEND_PATH: &str = "m/352'/1'/0'/0'/0";
const SCAN_PATH: &str = "m/352'/1'/0'/1'/0";
const SEGWIT_PATH: &str = "m/84'/1'/0'";
const NOT_A_BACKUP: &str = "expected a BIP138 encrypted backup of a BIP139 wallet backup or of \
                            a descriptor, a BIP139 wallet backup or a descriptor";

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

fn test_signer() -> HotSigner {
    HotSigner::new_from_mnemonics(bitcoin::Network::Regtest, TEST_MNEMONIC).expect("signer")
}

/// The test mnemonic's 32-byte scan secret at the BIP352 scan path of account 0, the key
/// an SP backup opens with.
fn scan_secret() -> [u8; 32] {
    let account = ChildNumber::from_hardened_idx(0).expect("account");
    let path = scan_path(bitcoin::Network::Regtest, account);
    assert_eq!(
        path.to_string(),
        SCAN_PATH.trim_start_matches("m/"),
        "the scan path"
    );
    test_signer().private_key_at(&path).secret_bytes()
}

/// The test mnemonic's xpub at the BIP352 spend path of account 0: its root is the spend
/// public key.
fn spend_xpub() -> String {
    let account = ChildNumber::from_hardened_idx(0).expect("account");
    let path = spend_path(bitcoin::Network::Regtest, account);
    assert_eq!(
        path.to_string(),
        SPEND_PATH.trim_start_matches("m/"),
        "the spend path"
    );
    let secp = Secp256k1::new();
    let child = test_signer()
        .master_xpriv()
        .derive_priv(&secp, &path)
        .expect("spend key");
    Xpub::from_priv(&secp, &child).to_string()
}

/// The x-only key BIP138 takes from `xpub`, its root.
fn xpub_key(xpub: &str) -> [u8; 32] {
    Xpub::from_str(xpub)
        .expect("xpub")
        .public_key
        .x_only_public_key()
        .0
        .serialize()
}

/// The SP key's descriptor, with a segwit sub-account of another key, seed 7.
fn create_sp(name: &str) -> String {
    let (fingerprint, xpub) = key(7, SEGWIT_PATH);
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

/// The keys of a Multikey policy: seeds 1 and 2 under musig(), seed 3 on a leaf, each
/// `[fingerprint/path]xpub`, with their fingerprints.
fn multikey_keys() -> Vec<(String, String)> {
    [1, 2, 3]
        .into_iter()
        .map(|seed| {
            let (fingerprint, xpub) = key(seed, "m/87'/1'/0'/2");
            (
                fingerprint.clone(),
                format!("[{fingerprint}/87'/1'/0'/2]{xpub}"),
            )
        })
        .collect()
}

fn multikey_descriptor() -> String {
    let keys = multikey_keys();
    format!(
        "tr(musig({},{})/<0;1>/*,pk({}/<2;3>/*))",
        keys[0].1, keys[1].1, keys[2].1
    )
}

/// A Multikey account of `multikey_descriptor()`, and its sorted fingerprints.
fn create_multikey(name: &str) -> Vec<String> {
    let created = create_musig_account(MusigCreationRequest {
        account_name: name.into(),
        network: Network::Regtest,
        descriptor: multikey_descriptor(),
        electrum_url: ELECTRUM.into(),
        verify_certificate: true,
    });
    assert!(
        created.is_ok,
        "create the Multikey account: {}",
        created.error
    );
    let mut fingerprints: Vec<_> = multikey_keys().into_iter().map(|key| key.0).collect();
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
#[derive(Debug, PartialEq)]
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

fn assert_restore_refused(restored: &BackupRestore, failure: BackupFailure, what: &str) {
    assert_eq!(
        (restored.is_ok, restored.failure),
        (false, failure),
        "{what}: {}",
        restored.error
    );
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
    let backup = account.backup();
    assert!(backup.is_ok, "back the SP account up: {}", backup.error);
    assert!(backup.excluded.is_empty(), "every SP key opens the backup");
    assert!(
        account.shutdown_backend().is_empty(),
        "close the SP account"
    );
    backup.bytes.into_iter().collect()
}

/// The BIP138 backup `bytes` hold, its clear part read.
fn encrypted(bytes: &[u8]) -> Encrypted {
    Encrypted::read(bytes)
        .expect("a BIP138 backup")
        .expect("its clear part reads")
}

/// The BIP139 document the backup `bytes` encrypts, opened with `key`.
fn document_of(bytes: &[u8], key: [u8; 32]) -> serde_json::Value {
    let items = encrypted(bytes)
        .open(&[key])
        .expect("not damaged")
        .expect("the key opens it");
    let documents: Vec<_> = items
        .iter()
        .filter(|(content, _)| *content == Content::Bip139)
        .collect();
    assert_eq!(documents.len(), 1, "one BIP139 document is encrypted");
    serde_json::from_slice(&documents[0].1).expect("the document is JSON")
}

// ===== Round Trips =====

#[test]
fn an_sp_backup_is_a_bip138_bip139_document_and_restores_its_indexes_and_labels() {
    let name = test_account_name();
    let segwit_fingerprint = create_sp(&name);
    let bytes = sp_backup(&name);
    let before = sp_state(&name);
    assert_eq!(before.addresses.len(), 2, "two segwit addresses handed out");

    let document = document_of(&bytes, scan_secret());
    let config = config_from_file(name.clone());
    assert_eq!(
        (
            &document["bip"],
            &document["version"],
            &document["name"],
            &document["network"]
        ),
        (
            &serde_json::json!(139),
            &serde_json::json!(1),
            &serde_json::json!(name),
            &serde_json::json!("regtest")
        ),
        "the wallet's BIP, version, name and network"
    );
    let accounts = document["accounts"].as_array().expect("accounts");
    assert_eq!(
        accounts.len(),
        2,
        "the SP account and its segwit sub-account"
    );
    assert_eq!(
        (
            &accounts[0]["type"],
            &accounts[0]["descriptor"],
            &accounts[0]["block_height"]
        ),
        (
            &serde_json::json!("bip_392"),
            &serde_json::json!(config.descriptor),
            &serde_json::json!(1)
        ),
        "the SP account: its full sp() descriptor and the block scanning starts from"
    );
    let txid = Txid::from_byte_array([9; 32]).to_string();
    let mut labels: Vec<_> = accounts[0]["bip329_labels"]
        .as_array()
        .expect("labels")
        .iter()
        .map(|label| {
            (
                label["type"].as_str().expect("type").to_string(),
                label["ref"].as_str().expect("ref").to_string(),
                label["label"].as_str().expect("label").to_string(),
            )
        })
        .collect();
    labels.sort();
    assert_eq!(
        labels,
        vec![
            ("addr".into(), before.addresses[0].clone(), "rent".into()),
            ("tx".into(), txid, "salary".into()),
        ],
        "the labels in BIP329"
    );
    assert_eq!(
        (
            &accounts[1]["type"],
            &accounts[1]["descriptor"],
            &accounts[1]["receive_index"],
            &accounts[1]["change_index"]
        ),
        (
            &serde_json::json!("bip_380"),
            &serde_json::json!(config.segwit_descriptor),
            &serde_json::json!(2),
            &serde_json::json!(0)
        ),
        "the segwit sub-account and its address indexes"
    );
    assert_eq!(
        document["proprietary"]["silent"],
        serde_json::json!({
            "plugin_id": "sp",
            "blindbit_url": BLINDBIT,
            "electrum_url": ELECTRUM,
            "verify_certificate": true,
            "dust_limit": 546,
        }),
        "the servers and settings only Silent reads"
    );

    let contents = inspect_account_backup(&bytes, vec![], TEST_MNEMONIC.into());
    assert!(contents.is_ok, "inspect the backup: {}", contents.error);
    assert_eq!(
        (
            contents.plugin_id.as_str(),
            contents.account.as_str(),
            contents.network,
            contents.network_known,
            contents.scan_start,
            contents.labels,
            contents.saved_psbts_excluded,
        ),
        ("sp", name.as_str(), Network::Regtest, true, 1, 2, false),
        "an SP backup's account, network, scan start and labels"
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
    let mut signers = vec![segwit_fingerprint, sp_key];
    signers.sort();
    assert_eq!(
        contents.signers, signers,
        "the SP key and the segwit key sign"
    );

    cleanup_test_account(&name);
    let restored = restore_account_backup(
        "sp",
        &bytes,
        vec![],
        TEST_MNEMONIC.into(),
        name.clone(),
        Network::Signet,
    );
    assert!(restored.is_ok, "restore the SP account: {}", restored.error);
    assert_eq!(restored.account, name, "restored under the name given");
    assert_eq!(
        config_from_file(name.clone()).network,
        Network::Regtest,
        "the backup's own network, not the one a restore picks for an unknown one"
    );
    assert_eq!(
        sp_state(&name),
        before,
        "the restored account handed out the same segwit addresses, with the same labels"
    );
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
    let backup = account.backup();
    assert!(
        backup.is_ok,
        "back the Multikey account up: {}",
        backup.error
    );
    assert!(
        backup.excluded.is_empty(),
        "every policy key opens the backup"
    );
    assert!(account.shutdown_backend().is_empty(), "close the account");
    let bytes: Vec<u8> = backup.bytes.into_iter().collect();

    // Any one key of the policy opens it, a musig() participant or a leaf key.
    let keys = multikey_keys();
    let document = document_of(&bytes, xpub_key(keys[2].1.split(']').nth(1).expect("xpub")));
    let accounts = document["accounts"].as_array().expect("accounts");
    assert_eq!(
        (
            accounts.len(),
            &accounts[0]["type"],
            &accounts[0]["descriptor"],
            &accounts[0]["receive_index"]
        ),
        (
            1,
            &serde_json::json!("bip_380"),
            &serde_json::json!(config.descriptor),
            &serde_json::json!(1)
        ),
        "one account: the policy and its receive index"
    );
    assert!(
        accounts[0].get("block_height").is_none(),
        "a Multikey account scans from no block"
    );
    let birthday = config
        .birthday_time
        .expect("a created account records its birthday");
    assert_eq!(
        accounts[0]["timestamp"],
        serde_json::json!(birthday),
        "the account's creation time is its BIP139 timestamp"
    );
    let contents = inspect_account_backup(&bytes, vec![keys[0].1.clone()], String::new());
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
    let restored = restore_account_backup(
        MUSIG_PLUGIN_ID,
        &bytes,
        vec![keys[1].1.clone()],
        String::new(),
        name.clone(),
        Network::Regtest,
    );
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
    assert_eq!(
        config.birthday_time,
        Some(birthday),
        "the restored account keeps the backup's birthday"
    );
    assert!(
        account.shutdown_backend().is_empty(),
        "close the restored account"
    );
    cleanup_test_account(&name);
}

// ===== Keys =====

#[test]
fn an_sp_backup_opens_with_the_scan_secret_and_never_with_a_public_key() {
    let name = test_account_name();
    create_sp(&name);
    let bytes = sp_backup(&name);
    let config = config_from_file(name.clone());
    cleanup_test_account(&name);

    let locked = inspect_account_backup(&bytes, vec![], String::new());
    assert_refused(&locked, BackupFailure::Locked, "no key");
    assert_eq!(
        locked.derivation_paths,
        vec![SCAN_PATH.to_string()],
        "a locked SP backup names the BIP352 scan path alone"
    );

    // The security property: the spend public key, which the account's SP address shows to
    // whoever is paid there, cannot decrypt the backup, nor can any xpub of the account.
    let sp = SpDescriptor::from_str(&config.descriptor).expect("sp() descriptor");
    let spend = sp
        .spend_public_key(&Secp256k1::new())
        .expect("the spend public key the SP address shows");
    let backup = encrypted(&bytes);
    assert!(
        backup
            .open(&[spend.x_only_public_key().0.serialize()])
            .expect("not damaged")
            .is_none(),
        "the spend public key of the SP address cannot decrypt the backup"
    );
    assert!(
        backup
            .open(&[scan_secret()])
            .expect("not damaged")
            .is_some(),
        "the scan secret decrypts it"
    );
    let (_, segwit) = key(7, SEGWIT_PATH);
    for (what, xpub) in [
        ("the spend path's xpub", spend_xpub()),
        ("the segwit sub-account's xpub", segwit.to_string()),
    ] {
        assert_refused(
            &inspect_account_backup(&bytes, vec![xpub], String::new()),
            BackupFailure::Locked,
            what,
        );
    }

    let opened = inspect_account_backup(&bytes, vec![], TEST_MNEMONIC.into());
    assert!(
        opened.is_ok,
        "the SP key's mnemonic opens it with its scan secret: {}",
        opened.error
    );
    // A key asked for its silent payment account answers its sp() descriptor.
    let opened = inspect_account_backup(&bytes, vec![config.descriptor.clone()], String::new());
    assert!(
        opened.is_ok,
        "the SP key's sp() descriptor opens it with its scan secret: {}",
        opened.error
    );
    assert_refused(
        &inspect_account_backup(
            &bytes,
            vec![descriptor_from_mnemonic(OTHER_MNEMONIC, Network::Regtest)],
            String::new(),
        ),
        BackupFailure::Locked,
        "another key's sp() descriptor",
    );
    assert_refused(
        &inspect_account_backup(&bytes, vec![], OTHER_MNEMONIC.into()),
        BackupFailure::Locked,
        "another key's mnemonic",
    );
    assert_refused(
        &inspect_account_backup(&bytes, vec!["tpubnotanxpub".into()], String::new()),
        BackupFailure::InvalidXpub,
        "a text that is no xpub",
    );
    assert_refused(
        &inspect_account_backup(&bytes, vec![], "abandon abandon abandon".into()),
        BackupFailure::InvalidMnemonic,
        "a text that is no mnemonic",
    );
    let restored = restore_account_backup(
        "sp",
        &bytes,
        vec![spend_xpub()],
        String::new(),
        name.clone(),
        Network::Regtest,
    );
    assert_restore_refused(
        &restored,
        BackupFailure::Locked,
        "the spend path's xpub restores nothing",
    );
    assert!(
        !config_from_file(name).is_ok(),
        "no account is left by a refused restore"
    );
}

// ===== Formats =====

#[test]
fn a_bip138_descriptor_restores_the_multikey_account_it_holds() {
    let name = test_account_name();
    let keys = multikey_keys();
    let (bytes, excluded) = encrypt_descriptor(&multikey_descriptor(), true).expect("encrypt");
    assert!(excluded.is_empty(), "every key encrypts");

    let contents = inspect_account_backup(&bytes, vec![keys[2].1.clone()], String::new());
    assert!(contents.is_ok, "inspect it: {}", contents.error);
    assert_eq!(
        (
            contents.plugin_id.as_str(),
            contents.account.as_str(),
            contents.network_known,
            contents.labels,
            contents.endpoints.len(),
        ),
        (MUSIG_PLUGIN_ID, "", false, 0, 0),
        "a descriptor names no account, no network, no label and no server"
    );
    let restored = restore_account_backup(
        MUSIG_PLUGIN_ID,
        &bytes,
        vec![keys[2].1.clone()],
        String::new(),
        name.clone(),
        Network::Regtest,
    );
    assert!(restored.is_ok, "restore it: {}", restored.error);
    let config = config_from_file(name.clone());
    assert_eq!(
        (
            config.plugin_id.as_str(),
            config.descriptor.as_str(),
            config.network
        ),
        (
            MUSIG_PLUGIN_ID,
            multikey_descriptor().as_str(),
            Network::Regtest
        ),
        "the policy restored on the network picked"
    );
    cleanup_test_account(&name);
}

#[test]
fn a_plain_bip139_document_restores_what_it_carries() {
    let name = test_account_name();
    let (fingerprint, xpub) = key(7, SEGWIT_PATH);
    let segwit = segwit_descriptor_for(fingerprint, xpub.to_string(), Network::Regtest);
    let document = serde_json::json!({
        "name": "from another wallet",
        "network": "regtest",
        "accounts": [
            {
                "type": "bip_392",
                "descriptor": descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
                "block_height": 500,
                "bip329_labels": [
                    { "type": "tx", "ref": Txid::from_byte_array([3; 32]).to_string(),
                      "label": "rent" },
                    { "type": "xpub", "ref": xpub.to_string(), "label": "not kept" },
                ],
            },
            { "type": "bip_380", "descriptor": segwit, "receive_index": 4, "change_index": 2 },
        ],
        "proprietary": { "silent": { "blindbit_url": BLINDBIT } },
    });
    let bytes = serde_json::to_vec_pretty(&document).expect("json");

    let contents = inspect_account_backup(&bytes, vec![], String::new());
    assert!(
        contents.is_ok,
        "a plain document needs no key: {}",
        contents.error
    );
    assert_eq!(
        (
            contents.plugin_id.as_str(),
            contents.account.as_str(),
            contents.network,
            contents.network_known,
            contents.scan_start,
            contents.labels,
        ),
        ("sp", "from another wallet", Network::Regtest, true, 500, 1),
        "an SP account named by the document, its scan start and the labels Silent keeps"
    );
    let tips: Vec<_> = contents
        .tips
        .iter()
        .map(|tip| (tip.kind.as_str(), tip.receive, tip.change))
        .collect();
    assert_eq!(tips, vec![("segwit", 4, 2)], "the sub-account's indexes");

    let restored = restore_account_backup(
        "sp",
        &bytes,
        vec![],
        String::new(),
        name.clone(),
        Network::Regtest,
    );
    assert!(restored.is_ok, "restore it: {}", restored.error);
    let config = config_from_file(name.clone());
    assert_eq!(
        (
            config.segwit_descriptor.as_str(),
            config.blindbit_url.as_str()
        ),
        (segwit.as_str(), BLINDBIT),
        "the sub-account and the server it names"
    );
    let state = sp_state(&name);
    assert_eq!(
        state.addresses.len(),
        4,
        "segwit addresses 1 to 4 handed out"
    );
    assert_eq!(
        state.labels,
        vec![(
            LabelKind::Transaction.repr,
            Txid::from_byte_array([3; 32]).to_string(),
            "rent".to_string()
        )],
        "the transaction label"
    );
    let contents = inspect_account_backup(&sp_backup(&name), vec![], TEST_MNEMONIC.into());
    assert!(contents.is_ok, "inspect its backup: {}", contents.error);
    assert_eq!(
        contents.scan_start, 500,
        "the restored account scans from the document's block"
    );
    cleanup_test_account(&name);
}

#[test]
fn a_plain_descriptor_restores_the_account_alone() {
    let name = test_account_name();
    let restored = restore_account_backup(
        MUSIG_PLUGIN_ID,
        format!("  {}\n", multikey_descriptor()).as_bytes(),
        vec![],
        String::new(),
        name.clone(),
        Network::Signet,
    );
    assert!(restored.is_ok, "restore a plain policy: {}", restored.error);
    let config = config_from_file(name.clone());
    assert_eq!(
        (
            config.network,
            config.electrum_url.as_str(),
            config.birthday_time
        ),
        (Network::Signet, "", None),
        "on the network picked, with no server named and no birthday"
    );
    cleanup_test_account(&name);

    // An SP account needs a BlindBit server: the one the network's last account used.
    app_set_last_endpoint(BackendServer::Blindbit, Network::Regtest, BLINDBIT.into());
    let descriptor = descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest);
    let contents = inspect_account_backup(descriptor.as_bytes(), vec![], String::new());
    assert!(contents.is_ok, "inspect a plain sp(): {}", contents.error);
    assert_eq!(
        (
            contents.plugin_id.as_str(),
            contents.network_known,
            contents.scan_start,
            contents.tips.len()
        ),
        ("sp", false, 0, 0),
        "test keys name no network, and a descriptor no scan start nor index"
    );
    let restored = restore_account_backup(
        "sp",
        descriptor.as_bytes(),
        vec![],
        String::new(),
        name.clone(),
        Network::Regtest,
    );
    assert!(restored.is_ok, "restore a plain sp(): {}", restored.error);
    let config = config_from_file(name.clone());
    assert_eq!(
        (
            config.descriptor.as_str(),
            config.blindbit_url.as_str(),
            config.scan_start
        ),
        (descriptor.as_str(), BLINDBIT, None),
        "the descriptor, the network's last BlindBit server and the default scan start"
    );
    cleanup_test_account(&name);
}

#[test]
fn a_file_in_none_of_the_formats_is_refused_naming_them() {
    let name = test_account_name();
    for (what, bytes) in [
        ("random bytes", vec![0xff, 0x00, 0x13, 0x37]),
        ("text", b"not a backup at all".to_vec()),
        ("JSON with no account", br#"{"accounts": []}"#.to_vec()),
        (
            "a password-sealed backup",
            b"SILENT-ACCOUNT-BACKUP\n{}".to_vec(),
        ),
    ] {
        let contents = inspect_account_backup(&bytes, vec![], String::new());
        assert_refused(&contents, BackupFailure::Unknown, what);
        assert!(
            contents.error.contains(NOT_A_BACKUP),
            "{what}: the refusal names the formats read, got {}",
            contents.error
        );
        let restored = restore_account_backup(
            "sp",
            &bytes,
            vec![],
            String::new(),
            name.clone(),
            Network::Regtest,
        );
        assert_restore_refused(&restored, BackupFailure::Unknown, what);
    }
    let (bytes, _) = encrypt_descriptor(&multikey_descriptor(), true).expect("encrypt");
    assert_refused(
        &inspect_account_backup(&bytes[..bytes.len() / 2], vec![], String::new()),
        BackupFailure::Malformed,
        "a cut BIP138 backup",
    );
    assert!(
        !config_from_file(name).is_ok(),
        "no account is left by a refused file"
    );
}

// ===== Restore Refusals =====

#[test]
fn a_restore_never_replaces_an_account_or_takes_another_kind() {
    let name = test_account_name();
    create_sp(&name);
    let bytes = sp_backup(&name);
    let before = sp_state(&name);

    let restored = restore_account_backup(
        "sp",
        &bytes,
        vec![],
        TEST_MNEMONIC.into(),
        name.clone(),
        Network::Regtest,
    );
    assert_restore_refused(
        &restored,
        BackupFailure::Exists,
        "an existing account is not restored over",
    );
    assert_eq!(sp_state(&name), before, "the existing account is untouched");

    cleanup_test_account(&name);
    let restored = restore_account_backup(
        MUSIG_PLUGIN_ID,
        &bytes,
        vec![],
        TEST_MNEMONIC.into(),
        name.clone(),
        Network::Regtest,
    );
    assert_restore_refused(
        &restored,
        BackupFailure::Unsupported,
        "an SP backup is not restored as a Multikey account",
    );
    assert!(
        !config_from_file(name.clone()).is_ok(),
        "nothing is created for another kind of account"
    );
}

#[test]
fn a_restore_failing_after_creation_leaves_no_account() {
    let name = test_account_name();
    let document = serde_json::json!({
        "accounts": [{
            "descriptor": multikey_descriptor(),
            "bip329_labels": [{ "type": "addr", "ref": "not an address", "label": "y" }],
        }],
    });
    let restored = restore_account_backup(
        MUSIG_PLUGIN_ID,
        &serde_json::to_vec(&document).expect("json"),
        vec![],
        String::new(),
        name.clone(),
        Network::Regtest,
    );
    assert_restore_refused(
        &restored,
        BackupFailure::Other,
        "the restore fails once the account is created",
    );
    assert!(
        !config_from_file(name).is_ok(),
        "the account it created is removed"
    );
}

#[test]
fn an_sp_backup_records_the_scan_start_bwk_persisted() {
    let name = test_account_name();
    create_sp(&name);
    // The store is seeded at block 1: a scan start the config gains later seeds nothing.
    sp_backup(&name);
    let mut config = config_from_file(name.clone());
    config.scan_start = Some(700);
    assert_eq!(config.save(), "", "save the config");

    let document = document_of(&sp_backup(&name), scan_secret());
    assert_eq!(
        document["accounts"][0]["block_height"],
        serde_json::json!(1),
        "the backup records the persisted start"
    );
    cleanup_test_account(&name);
}

#[test]
fn a_bip139_document_parses_with_its_aliases() {
    let document = WalletBackup::parse(
        br#"{"alias": "w", "accounts": [{"alias": "a", "labels": [{"type": "tx", "ref": "r"}]}]}"#,
    )
    .expect("parse");
    assert_eq!(
        (
            document.name.as_deref(),
            document.accounts[0].name.as_deref(),
            document.accounts[0]
                .bip329_labels
                .as_ref()
                .map(|labels| labels.len())
        ),
        (Some("w"), Some("a"), Some(1)),
        "alias names the wallet and an account, labels are the BIP329 labels"
    );
}
