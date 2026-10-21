mod common;

use std::{fs, thread};

use silent::{
    config_from_file, create_musig_account, create_sp_account, AccountCreationResult,
    MusigCreationRequest, Network, SpCreationRequest,
};

fn request(name: &str) -> SpCreationRequest {
    SpCreationRequest {
        account_name: name.into(),
        network: Network::Regtest,
        descriptor: common::descriptor_from_mnemonic(common::TEST_MNEMONIC, Network::Regtest),
        blindbit_url: "http://localhost:8080".into(),
        electrum_url: "tcp://localhost:50001".into(),
        ..Default::default()
    }
}

fn assert_failed(result: AccountCreationResult) {
    assert!(!result.is_ok, "creation must fail");
    assert!(
        !result.error.is_empty(),
        "creation failure must carry an error"
    );
    assert!(
        result.preference_error.is_empty(),
        "no preference write after failed creation"
    );
}

#[test]
fn creation_validation_exclusive_write_and_preference_failure() {
    common::isolated_state();
    for name in ["", "has space", "line\nbreak", " leading"] {
        assert_failed(create_sp_account(request(name)));
    }
    let mut invalid = request("bad-descriptor");
    invalid.descriptor = "invalid".into();
    assert_failed(create_sp_account(invalid));
    let mut invalid = request("bad-network");
    invalid.network = Network::Bitcoin;
    assert_failed(create_sp_account(invalid));
    let mut invalid = request("bad-endpoint");
    invalid.electrum_url = "unknown://localhost:50001".into();
    assert_failed(create_sp_account(invalid));
    let mut invalid = request("bad-blindbit");
    invalid.blindbit_url = "file:///tmp".into();
    assert_failed(create_sp_account(invalid));
    let mut invalid = request("bad-subaccount");
    invalid.taproot_descriptor = "invalid".into();
    assert_failed(create_sp_account(invalid));

    let name = format!("exclusive_{}", std::process::id());
    let first = request(&name);
    let second = first.clone();
    let a = thread::spawn(move || create_sp_account(first));
    let b = thread::spawn(move || create_sp_account(second));
    let results = [
        a.join().expect("first creator"),
        b.join().expect("second creator"),
    ];
    assert_eq!(
        results.iter().filter(|r| r.is_ok).count(),
        1,
        "exactly one creator wins"
    );
    assert!(
        !results
            .iter()
            .find(|r| !r.is_ok)
            .expect("loser")
            .error
            .is_empty(),
        "loser reports failure"
    );
    let saved = config_from_file(name.clone());
    assert!(saved.is_ok(), "winner wrote a readable config");
    assert_eq!(
        saved.get_descriptor(),
        request(&name).descriptor,
        "saved descriptor unchanged"
    );
    assert_failed(create_sp_account(request(&name)));

    let secp = bitcoin::secp256k1::Secp256k1::new();
    let master = bitcoin::bip32::Xpriv::new_master(bitcoin::Network::Regtest, &[19; 32])
        .expect("test master");
    let descriptor = format!(
        "tr([{}]{}/<0;1>/*)",
        master.fingerprint(&secp),
        bitcoin::bip32::Xpub::from_priv(&secp, &master)
    );
    let multikey = MusigCreationRequest {
        account_name: "multikey".into(),
        network: Network::Regtest,
        descriptor,
        electrum_url: "ssl://localhost:50002".into(),
        verify_certificate: false,
    };
    let result = create_musig_account(multikey.clone());
    assert!(result.is_ok, "create Multikey: {}", result.error);
    let saved = config_from_file("multikey".into());
    assert_eq!(saved.get_plugin_id(), "musig", "Multikey module");
    assert!(
        !saved.get_verify_certificate(),
        "explicit certificate policy"
    );
    assert_eq!(
        silent::list_module_configs("sp"),
        vec![name.clone()],
        "SP discovery filters without reopening configs"
    );
    assert_eq!(
        silent::list_module_configs("musig"),
        vec!["multikey".to_string()],
        "Multikey discovery"
    );
    assert!(
        silent::list_module_configs("missing-plugin").is_empty(),
        "unknown module has no accounts"
    );
    assert_failed(create_musig_account(multikey.clone()));
    let mut wrong_network = multikey.clone();
    wrong_network.account_name = "multikey-wrong-network".into();
    wrong_network.network = Network::Bitcoin;
    assert_failed(create_musig_account(wrong_network));
    let mut invalid = multikey;
    invalid.account_name = "multikey-invalid".into();
    invalid.descriptor = "invalid".into();
    assert_failed(create_musig_account(invalid));

    // A failed preference replacement must not undo a successfully written account.
    let app = silent::config::datadir().join("app.bip138");
    assert!(app.is_file(), "successful creation remembered endpoints");
    fs::remove_file(&app).expect("remove this test's preferences");
    fs::create_dir(&app).expect("block preference replacement");
    let result = create_sp_account(request("preference-failure"));
    assert!(result.is_ok, "account creation succeeded: {}", result.error);
    assert!(
        !result.preference_error.is_empty(),
        "separate preference failure"
    );
    assert!(
        config_from_file("preference-failure".into()).is_ok(),
        "account survived preference failure"
    );
    let root = silent::config::datadir();
    let saved_root = root.with_extension("creation-backup");
    fs::rename(&root, &saved_root).expect("retain this test's state directory");
    fs::write(&root, b"not a directory").expect("block creation filesystem path");
    let failure = create_sp_account(request("io-failure"));
    fs::remove_file(&root).expect("remove test filesystem blocker");
    fs::rename(&saved_root, &root).expect("restore test state directory");
    assert_failed(failure);
    assert!(
        config_from_file(name).is_ok(),
        "I/O failure did not overwrite an existing account"
    );
}
