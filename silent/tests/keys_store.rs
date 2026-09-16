//! Integration tests for the on-disk key store: file layout and permissions, full
//! round trips, per-network isolation, the invariants layered on top of `KeyStore`,
//! and refusal of a corrupt, future-version or mismatched file.

use std::str::FromStr;

use bitcoin::bip32::{DerivationPath, Fingerprint, Xpub};
use bitcoin::Network;
use silent::keys::{
    DescriptorEntry, DeviceRecord, Error, KeyManager, KeyManagerImpl, KeyRecord, KeyStore,
    RouteKind, Routes, SecretEnvelope, XpubEntry, KEYS_DIR_NAME, KEY_RECORD_VERSION,
};

static DATADIR: std::sync::OnceLock<temp_dir::TempDir> = std::sync::OnceLock::new();

/// One datadir for this binary. Holding the TempDir here keeps it alive until the
/// process exits; dropping it would delete the tree under the running tests.
fn datadir() -> &'static std::path::Path {
    DATADIR
        .get_or_init(|| {
            let tmp = temp_dir::TempDir::new().expect("create temp dir");
            silent::set_datadir(tmp.path().to_path_buf());
            tmp
        })
        .path()
}

fn store(network: Network) -> KeyStore {
    KeyStore::new(datadir().join(KEYS_DIR_NAME), network)
}

fn manager(network: Network) -> KeyManager {
    KeyManager::new(store(network), network).expect("build manager")
}

fn fingerprint(value: u32) -> Fingerprint {
    Fingerprint::from(value.to_be_bytes())
}

fn test_xpub(seed_byte: u8) -> Xpub {
    let seed = [seed_byte; 32];
    let xpriv =
        bitcoin::bip32::Xpriv::new_master(bitcoin::NetworkKind::Test, &seed).expect("valid seed");
    let secp = bitcoin::secp256k1::Secp256k1::new();
    Xpub::from_priv(&secp, &xpriv)
}

fn secret_envelope() -> SecretEnvelope {
    SecretEnvelope {
        version: 1,
        kdf: "argon2id".to_string(),
        m_cost_kib: 65_536,
        t_cost: 3,
        p_cost: 1,
        salt: "salt".to_string(),
        cipher: "xchacha20poly1305".to_string(),
        nonce: "nonce".to_string(),
        ciphertext: "ciphertext".to_string(),
    }
}

// ===== Layout =====

#[test]
fn test_empty_datadir_creates_nothing() {
    // Bitcoin mainnet is not touched by any other test in this file, so its
    // directory is guaranteed to still be absent here.
    let key_store = store(Network::Bitcoin);
    let records = key_store
        .list()
        .expect("list should succeed on a network with no records");
    assert!(
        records.is_empty(),
        "expected an empty list for a fresh network, got {records:?}"
    );
    let dir = key_store.dir().expect("network dir path");
    assert!(
        !dir.exists(),
        "listing must not create the network directory, but {} exists",
        dir.display()
    );
}

#[cfg(unix)]
#[test]
fn test_file_layout_and_permissions() {
    use std::os::unix::fs::PermissionsExt;

    let fp = fingerprint(1);
    let key_store = store(Network::Regtest);
    key_store
        .save(&KeyRecord::new(
            fp,
            Network::Regtest,
            "layout key".to_string(),
        ))
        .expect("save should succeed");

    let expected_path = datadir()
        .join(KEYS_DIR_NAME)
        .join("regtest")
        .join(format!("{fp}.json"));
    let actual_path = key_store.record_path(fp).expect("record path");
    assert_eq!(
        actual_path,
        expected_path,
        "expected the record at the documented layout path, got {}",
        actual_path.display()
    );

    let file_mode = std::fs::metadata(&expected_path)
        .expect("file metadata")
        .permissions()
        .mode()
        & 0o777;
    assert_eq!(
        file_mode, 0o600,
        "expected the record file mode to be 0o600, got {file_mode:o}"
    );

    let network_dir = expected_path.parent().expect("network dir");
    let network_dir_mode = std::fs::metadata(network_dir)
        .expect("network dir metadata")
        .permissions()
        .mode()
        & 0o777;
    assert_eq!(
        network_dir_mode, 0o700,
        "expected the network dir mode to be 0o700, got {network_dir_mode:o}"
    );

    let keys_dir = datadir().join(KEYS_DIR_NAME);
    let keys_dir_mode = std::fs::metadata(&keys_dir)
        .expect("keys dir metadata")
        .permissions()
        .mode()
        & 0o777;
    assert_eq!(
        keys_dir_mode, 0o700,
        "expected the keys dir mode to be 0o700, got {keys_dir_mode:o}"
    );
}

// ===== Round Trip =====

#[test]
fn test_record_round_trip_every_field() {
    let fp = fingerprint(2);
    let key_store = store(Network::Regtest);

    let mut record = KeyRecord::new(fp, Network::Regtest, "full record alias".to_string());
    record.routes = Routes::try_from(vec![
        RouteKind::Hot,
        RouteKind::Usb,
        RouteKind::Qr,
        RouteKind::SdCard,
    ])
    .expect("valid route order");
    record.devices.push(DeviceRecord {
        kind: "hardware".to_string(),
        model: "model-a".to_string(),
        serial: "serial-a".to_string(),
        module: "hwi".to_string(),
    });
    record.devices.push(DeviceRecord {
        kind: "hardware".to_string(),
        model: "model-b".to_string(),
        serial: "serial-b".to_string(),
        module: "hwi".to_string(),
    });
    record.xpubs.push(XpubEntry {
        path: DerivationPath::from_str("m/86'/1'/0'").expect("valid path"),
        xpub: test_xpub(1),
    });
    record.xpubs.push(XpubEntry {
        path: DerivationPath::from_str("m/86'/1'/1'").expect("valid path"),
        xpub: test_xpub(2),
    });
    record.descriptors.push(DescriptorEntry {
        alias: "descriptor one".to_string(),
        descriptor: "descriptor string one".to_string(),
    });
    record.descriptors.push(DescriptorEntry {
        alias: "descriptor two".to_string(),
        descriptor: "descriptor string two".to_string(),
    });
    record.secret = Some(secret_envelope());

    key_store.save(&record).expect("save should succeed");
    let loaded = key_store.load(fp).expect("load should succeed");

    assert_eq!(
        loaded.version, 1,
        "expected the literal current record version 1, got {}",
        loaded.version
    );
    assert_eq!(
        loaded.fingerprint, record.fingerprint,
        "fingerprint mismatch after round trip, got {}",
        loaded.fingerprint
    );
    assert_eq!(
        loaded.network, record.network,
        "network mismatch after round trip, got {:?}",
        loaded.network
    );
    assert_eq!(
        loaded.alias, record.alias,
        "alias mismatch after round trip, got {}",
        loaded.alias
    );
    assert_eq!(
        loaded.routes.as_slice(),
        record.routes.as_slice(),
        "routes mismatch after round trip, got {:?}",
        loaded.routes.as_slice()
    );
    assert_eq!(
        loaded.devices, record.devices,
        "devices mismatch after round trip, got {:?}",
        loaded.devices
    );
    assert_eq!(
        loaded.xpubs, record.xpubs,
        "xpubs mismatch after round trip, got {:?}",
        loaded.xpubs
    );
    assert_eq!(
        loaded.descriptors, record.descriptors,
        "descriptors mismatch after round trip, got {:?}",
        loaded.descriptors
    );
    assert_eq!(
        loaded.secret, record.secret,
        "secret envelope mismatch after round trip, got {:?}",
        loaded.secret
    );
}

#[test]
fn test_same_fingerprint_on_two_networks_is_two_records() {
    let fp = fingerprint(3);
    let signet_store = store(Network::Signet);
    let regtest_store = store(Network::Regtest);

    signet_store
        .save(&KeyRecord::new(
            fp,
            Network::Signet,
            "signet key".to_string(),
        ))
        .expect("save signet record");
    regtest_store
        .save(&KeyRecord::new(
            fp,
            Network::Regtest,
            "regtest key".to_string(),
        ))
        .expect("save regtest record");

    let signet_loaded = signet_store.load(fp).expect("load signet record");
    let regtest_loaded = regtest_store.load(fp).expect("load regtest record");
    assert_eq!(
        signet_loaded.alias, "signet key",
        "expected the signet alias, got {}",
        signet_loaded.alias
    );
    assert_eq!(
        regtest_loaded.alias, "regtest key",
        "expected the regtest alias, got {}",
        regtest_loaded.alias
    );

    let signet_path = signet_store.record_path(fp).expect("signet path");
    let regtest_path = regtest_store.record_path(fp).expect("regtest path");
    assert_ne!(
        signet_path, regtest_path,
        "expected different paths for the same fingerprint on different networks"
    );

    regtest_store.delete(fp).expect("delete regtest record");
    assert!(
        signet_path.exists(),
        "expected the signet record to remain after deleting the regtest one"
    );
    let regtest_after = regtest_store.load(fp);
    assert!(
        matches!(regtest_after, Err(Error::NotFound(found)) if found == fp),
        "expected NotFound after deleting the regtest record, got {regtest_after:?}"
    );
}

// ===== Invariants =====

#[test]
fn test_duplicate_add_key_is_refused() {
    let fp = fingerprint(4);
    let mut key_manager = manager(Network::Regtest);
    key_manager
        .add_key(KeyRecord::new(
            fp,
            Network::Regtest,
            "first alias".to_string(),
        ))
        .expect("first add should succeed");

    let result = key_manager.add_key(KeyRecord::new(
        fp,
        Network::Regtest,
        "second alias".to_string(),
    ));
    assert!(
        matches!(result, Err(Error::DuplicateKey(found)) if found == fp),
        "expected DuplicateKey on the second add, got {result:?}"
    );

    let key_store = store(Network::Regtest);
    let loaded = key_store.load(fp).expect("load should succeed");
    assert_eq!(
        loaded.alias, "first alias",
        "expected the first alias to remain unchanged on disk, got {}",
        loaded.alias
    );
}

#[test]
fn test_device_dedupe() {
    let fp = fingerprint(5);
    let mut key_manager = manager(Network::Regtest);
    key_manager
        .add_key(KeyRecord::new(
            fp,
            Network::Regtest,
            "device key".to_string(),
        ))
        .expect("add key");

    let device = DeviceRecord {
        kind: "hardware".to_string(),
        model: "model-x".to_string(),
        serial: "serial-x".to_string(),
        module: "hwi".to_string(),
    };
    key_manager
        .record_device(fp, device.clone())
        .expect("first record");
    key_manager
        .record_device(fp, device.clone())
        .expect("duplicate record is a no-op");

    let after_duplicate = key_manager.key(fp).expect("key present").devices.len();
    assert_eq!(
        after_duplicate, 1,
        "expected the duplicate device to be a no-op, got {after_duplicate}"
    );

    let mut other_device = device.clone();
    other_device.serial = "serial-y".to_string();
    key_manager
        .record_device(fp, other_device)
        .expect("differing device records");

    let after_second = key_manager.key(fp).expect("key present").devices.len();
    assert_eq!(
        after_second, 2,
        "expected a device differing in serial to add a new entry, got {after_second}"
    );
}

#[test]
fn test_route_invariants() {
    let fp = fingerprint(6);
    let key_store = store(Network::Regtest);
    let mut record = KeyRecord::new(fp, Network::Regtest, "route key".to_string());
    record.secret = Some(secret_envelope());
    key_store.save(&record).expect("save the initial record");

    record.routes.push(RouteKind::Usb).expect("push usb");
    record.routes.push(RouteKind::Qr).expect("push qr");
    record.routes.push(RouteKind::Hot).expect("push hot");
    assert_eq!(
        record.routes.first(),
        Some(RouteKind::Hot),
        "expected hot first regardless of insertion order, got {:?}",
        record.routes.as_slice()
    );

    let duplicate = record.routes.push(RouteKind::Usb);
    assert!(
        matches!(duplicate, Err(Error::DuplicateRoute(RouteKind::Usb))),
        "expected DuplicateRoute(Usb) when pushing usb twice, got {duplicate:?}"
    );

    let bad_order = record.routes.set_order(vec![RouteKind::SdCard]);
    assert!(
        matches!(bad_order, Err(Error::RouteOrderMismatch)),
        "expected RouteOrderMismatch for an order naming a route the key does not have, got {bad_order:?}"
    );

    key_store
        .save(&record)
        .expect("save with hot, usb and qr routes");

    record.routes.remove(RouteKind::Hot).expect("remove hot");
    record.routes.remove(RouteKind::Usb).expect("remove usb");
    record.routes.remove(RouteKind::Qr).expect("remove qr");
    assert!(
        record.routes.is_empty(),
        "expected an empty route list after removing every route, got {:?}",
        record.routes.as_slice()
    );

    key_store.save(&record).expect("save with no routes");
    let loaded = key_store.load(fp).expect("load should succeed");
    assert!(
        loaded.routes.is_empty(),
        "expected the loaded record to have no routes, got {:?}",
        loaded.routes.as_slice()
    );
}

#[test]
fn test_hot_route_refused_without_secret() {
    let fp = fingerprint(7);
    let key_store = store(Network::Regtest);
    let mut record = KeyRecord::new(fp, Network::Regtest, "no secret key".to_string());
    record.routes.push(RouteKind::Usb).expect("push usb");
    key_store.save(&record).expect("save the usb-only record");

    record
        .routes
        .push(RouteKind::Hot)
        .expect("push hot in memory");
    let result = key_store.save(&record);
    assert!(
        matches!(result, Err(Error::NoSecret(found)) if found == fp),
        "expected NoSecret when saving a hot route with no secret, got {result:?}"
    );

    let on_disk = key_store.load(fp).expect("load should succeed");
    assert!(
        !on_disk.routes.contains(RouteKind::Hot),
        "expected the on-disk record to still have no hot route, got {:?}",
        on_disk.routes.as_slice()
    );
    assert_eq!(
        on_disk.routes.as_slice(),
        &[RouteKind::Usb],
        "expected the on-disk record to be unchanged, got {:?}",
        on_disk.routes.as_slice()
    );
}

#[test]
fn test_hot_route_refused_without_secret_through_manager() {
    // Uses Testnet rather than Regtest so this does not contend with the many
    // Regtest-network tests in this file for the same on-disk network directory.
    let fp = fingerprint(13);
    let mut key_manager = manager(Network::Testnet);
    key_manager
        .add_key(KeyRecord::new(
            fp,
            Network::Testnet,
            "no secret key".to_string(),
        ))
        .expect("add the secret-less key");

    let result = key_manager.add_route(fp, RouteKind::Hot);
    assert!(
        matches!(result, Err(Error::NoSecret(found)) if found == fp),
        "expected NoSecret when adding a hot route through the manager with no secret, got {result:?}"
    );

    let on_disk = store(Network::Testnet)
        .load(fp)
        .expect("load should succeed");
    assert!(
        !on_disk.routes.contains(RouteKind::Hot),
        "expected the on-disk record to still have no hot route after the refused add_route, got {:?}",
        on_disk.routes.as_slice()
    );
}

// ===== Refusals =====

#[test]
fn test_corrupt_file_is_skipped_when_listing_and_errors_on_load() {
    let good_fp = fingerprint(8);
    let corrupt_fp = fingerprint(9);
    let key_store = store(Network::Regtest);
    key_store
        .save(&KeyRecord::new(
            good_fp,
            Network::Regtest,
            "good key".to_string(),
        ))
        .expect("save the good record");

    let corrupt_path = key_store
        .record_path(corrupt_fp)
        .expect("corrupt record path");
    std::fs::write(&corrupt_path, "{ not json").expect("write corrupt file");
    let corrupt_bytes_before = std::fs::read(&corrupt_path).expect("read corrupt file");

    let listed = key_store.list().expect("list should succeed");
    assert!(
        listed.iter().any(|record| record.fingerprint == good_fp),
        "expected the good record in the listing, got {listed:?}"
    );
    assert!(
        !listed.iter().any(|record| record.fingerprint == corrupt_fp),
        "expected the corrupt record to be skipped, got {listed:?}"
    );

    let load_result = key_store.load(corrupt_fp);
    assert!(
        matches!(load_result, Err(Error::Parse { .. })),
        "expected a Parse error loading the corrupt file, got {load_result:?}"
    );

    let corrupt_bytes_after = std::fs::read(&corrupt_path).expect("read corrupt file again");
    assert_eq!(
        corrupt_bytes_after, corrupt_bytes_before,
        "expected the corrupt file to be byte for byte unchanged"
    );
}

#[test]
fn test_future_version_is_refused_and_not_rewritten() {
    let fp = fingerprint(10);
    let key_store = store(Network::Regtest);
    let mut record = KeyRecord::new(fp, Network::Regtest, "future key".to_string());
    // Saving through the store creates the network dir with the store's mode.
    key_store
        .save(&record)
        .expect("save current-version record");
    record.version = KEY_RECORD_VERSION + 1;

    let path = key_store.record_path(fp).expect("record path");
    let bytes = serde_json::to_vec_pretty(&record).expect("serialize record");
    std::fs::write(&path, &bytes).expect("write future-version record");

    let load_result = key_store.load(fp);
    assert!(
        matches!(
            load_result,
            Err(Error::UnsupportedRecordVersion { found, supported })
                if found == KEY_RECORD_VERSION + 1 && supported == KEY_RECORD_VERSION
        ),
        "expected UnsupportedRecordVersion({}) on load, got {load_result:?}",
        KEY_RECORD_VERSION + 1
    );
    let load_message = key_store.load(fp).unwrap_err().to_string();
    assert!(
        load_message.contains(&(KEY_RECORD_VERSION + 1).to_string()),
        "expected the error message to mention the unsupported version, got: {load_message}"
    );

    let save_result = key_store.save(&record);
    assert!(
        matches!(
            save_result,
            Err(Error::UnsupportedRecordVersion { found, supported })
                if found == KEY_RECORD_VERSION + 1 && supported == KEY_RECORD_VERSION
        ),
        "expected UnsupportedRecordVersion on save, got {save_result:?}"
    );

    let bytes_after = std::fs::read(&path).expect("read file back");
    assert_eq!(
        bytes_after, bytes,
        "the file on disk must be unchanged after the refused load and save"
    );
}

#[test]
fn test_filename_must_match_the_record_fingerprint() {
    let fp_a = fingerprint(11);
    let fp_b = fingerprint(12);
    let key_store = store(Network::Regtest);
    let record = KeyRecord::new(fp_a, Network::Regtest, "mismatched key".to_string());
    // Saving through the store creates the network dir with the store's mode.
    key_store.save(&record).expect("save record a");

    let wrong_path = key_store.record_path(fp_b).expect("record path for b");
    let bytes = serde_json::to_vec_pretty(&record).expect("serialize record");
    std::fs::write(&wrong_path, &bytes).expect("write mismatched record");

    let load_result = key_store.load(fp_b);
    assert!(
        matches!(
            load_result,
            Err(Error::FingerprintMismatch { expected, found, .. })
                if expected == fp_b && found == fp_a
        ),
        "expected FingerprintMismatch naming both {fp_b} and {fp_a}, got {load_result:?}"
    );
    let message = key_store.load(fp_b).unwrap_err().to_string();
    assert!(
        message.contains(&fp_b.to_string()) && message.contains(&fp_a.to_string()),
        "expected the error message to name both the filename fingerprint {fp_b} and the \
         record's own {fp_a}, got: {message}"
    );
}
