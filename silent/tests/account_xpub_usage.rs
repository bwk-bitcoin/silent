mod common;

use std::{fs, str::FromStr};

use bitcoin::{
    bip32::{DerivationPath, Fingerprint, Xpriv, Xpub},
    secp256k1::Secp256k1,
};
use silent::{
    config::datadir,
    config_from_file, create_musig_account, delete_musig_account,
    keys::{Error, KeyManager, KeyManagerImpl, KeyRecord, KeyStore, KEYS_DIR_NAME},
    MusigCreationRequest, Network,
};

struct CachedAccount {
    request: MusigCreationRequest,
    manager: KeyManager,
    fingerprints: Vec<Fingerprint>,
    path: DerivationPath,
}

fn cached_account() -> CachedAccount {
    common::isolated_state();
    let network = bitcoin::Network::Regtest;
    let mut manager = KeyManager::new(
        KeyStore::new(datadir().join(KEYS_DIR_NAME), network),
        network,
    )
    .expect("open isolated key store");
    let secp = Secp256k1::new();
    let path = DerivationPath::from_str("m/87'/1'/0'").expect("cosigner path");
    let mut participants = Vec::new();
    let mut fingerprints = Vec::new();
    for seed in [41, 42] {
        let master = Xpriv::new_master(network, &[seed; 32]).expect("fixture master");
        let fingerprint = master.fingerprint(&secp);
        let child = master.derive_priv(&secp, &path).expect("fixture cosigner");
        let xpub = Xpub::from_priv(&secp, &child);
        manager
            .add_key(KeyRecord::new(fingerprint, network, format!("key-{seed}")))
            .expect("register cosigner without a device or descriptor registration");
        manager
            .cache_xpub(fingerprint, path.clone(), xpub)
            .expect("cache unused cosigner xpub");
        participants.push(format!("[{fingerprint}/87'/1'/0']{xpub}"));
        fingerprints.push(fingerprint);
    }
    CachedAccount {
        request: MusigCreationRequest {
            account_name: common::test_account_name(),
            network: Network::Regtest,
            descriptor: format!("tr(musig({})/<0;1>/*)", participants.join(",")),
            electrum_url: String::new(),
            verify_certificate: true,
        },
        manager,
        fingerprints,
        path,
    }
}

#[test]
fn immediate_account_deletion_before_discovery_keeps_xpubs_used() {
    if common::logical::isolated_process(
        "immediate_account_deletion_before_discovery_keeps_xpubs_used",
    ) {
        return;
    }
    let mut fixture = cached_account();
    let created = create_musig_account(fixture.request.clone());
    assert!(created.is_ok, "create MuSig account: {}", created.error);
    let deleted = delete_musig_account(fixture.request.account_name.clone());
    assert!(
        deleted.is_empty(),
        "immediately delete closed account: {deleted}"
    );
    assert!(
        !config_from_file(fixture.request.account_name).is_ok(),
        "config is erased before any account usage discovery"
    );

    for fingerprint in &fixture.fingerprints {
        let removed = fixture.manager.remove_xpub(*fingerprint, &fixture.path);
        assert!(
            matches!(removed, Err(Error::XpubInUse { .. })),
            "stale pre-creation manager must not remove used cosigner {fingerprint}: {removed:?}"
        );
    }
    fixture.manager.reload().expect("reload persistent usage");
    for fingerprint in fixture.fingerprints {
        let record = fixture.manager.key(fingerprint).expect("cosigner remains");
        assert!(
            record.descriptors.is_empty(),
            "usage does not depend on optional device registration"
        );
        assert!(
            record
                .xpubs
                .iter()
                .any(|entry| entry.path == fixture.path && entry.used),
            "cosigner {fingerprint} remains used after account deletion and reload"
        );
    }
}

#[test]
fn account_creation_propagates_xpub_usage_write_failure() {
    if common::logical::isolated_process("account_creation_propagates_xpub_usage_write_failure") {
        return;
    }
    let fixture = cached_account();
    let lock = datadir().join(KEYS_DIR_NAME).join("regtest/.lock");
    assert!(lock.is_file(), "fixture initialized its key-store lock");
    fs::remove_file(&lock).expect("remove isolated fixture lock");
    fs::create_dir(&lock).expect("prevent key-store write locking");

    let created = create_musig_account(fixture.request.clone());
    assert!(
        !created.is_ok,
        "usage persistence failure refuses account creation"
    );
    assert!(
        created.error.starts_with("mark account '"),
        "return the usage write failure: {}",
        created.error
    );
    assert!(
        created.preference_error.is_empty(),
        "failed creation never writes preferences"
    );
    assert!(
        !config_from_file(fixture.request.account_name).is_ok(),
        "failed usage write must not publish the account config"
    );
    let reloaded = KeyManager::new(
        KeyStore::new(datadir().join(KEYS_DIR_NAME), bitcoin::Network::Regtest),
        bitcoin::Network::Regtest,
    )
    .expect("read key store after failed write");
    for fingerprint in fixture.fingerprints {
        let record = reloaded.key(fingerprint).expect("cosigner remains");
        assert!(
            record.xpubs.iter().all(|entry| !entry.used),
            "failed first usage write leaves cosigner {fingerprint} unused"
        );
    }
}
