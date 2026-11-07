mod common;

use std::{io::ErrorKind, net::TcpListener};

use bitcoin::{
    bip32::{Xpriv, Xpub},
    secp256k1::Secp256k1,
};
use silent::{BackendErrorKind, BackendServer, Config, EndpointPhase, Network};

fn config(musig: bool, endpoint: &str) -> Box<Config> {
    let name = common::test_account_name();
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(bitcoin::Network::Signet, &[219; 32]).expect("test master key");
    let descriptor = silent::validate_musig_descriptor(format!(
        "tr({}/<0;1>/*)",
        Xpub::from_priv(&secp, &master)
    ))
    .descriptor;
    let mut config = silent::new_config(
        name,
        Network::Signet,
        if musig {
            descriptor.clone()
        } else {
            common::descriptor_from_mnemonic(common::TEST_MNEMONIC, Network::Signet)
        },
        "http://127.0.0.1:1".into(),
        endpoint.into(),
        546,
        if musig { "musig" } else { "sp" }.into(),
    );
    if !musig {
        config.set_taproot_descriptor(descriptor);
    }
    assert!(config.save().is_empty(), "save isolated opening config");
    config
}

#[test]
fn sp_offline_opening_never_contacts_saved_endpoint_and_failed_opening_can_recover() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("local saved endpoint");
    listener
        .set_nonblocking(true)
        .expect("nonblocking listener");
    let mut config = config(
        false,
        &format!("tcp://{}", listener.local_addr().expect("peer address")),
    );
    let mut account = silent::new_account_offline(&config);
    assert!(account.is_ok(), "open SP offline: {}", account.get_error());
    assert_eq!(account.network(), Network::Signet, "actual SP network");
    assert_eq!(
        account.descriptor(),
        config.get_descriptor(),
        "actual SP descriptor"
    );
    assert_eq!(
        account.sub_account_count(),
        1,
        "real Electrum sub-account exists"
    );
    assert_eq!(
        listener
            .accept()
            .expect_err("offline opening must not connect")
            .kind(),
        ErrorKind::WouldBlock
    );
    account.close_backend();
    drop(account);

    let descriptor = config.get_descriptor();
    config.set_descriptor("invalid descriptor".into());
    assert!(
        config.save().is_empty(),
        "persist invalid descriptor fixture"
    );
    let failed = silent::new_account_offline(&config);
    assert!(!failed.is_ok(), "invalid initial offline opening fails");
    config.set_descriptor(descriptor);
    assert!(config.save().is_empty(), "restore valid SP config");
    let mut change = silent::sp_endpoint_change(failed, BackendServer::Electrum, false);
    change.prepare_endpoint();
    assert_eq!(
        change.endpoint_phase(),
        EndpointPhase::Draining,
        "failed opening retained the requested name"
    );
    change.finish_endpoint_after_drain();
    assert!(
        change.endpoint_result().is_ok,
        "fresh command recovers: {}",
        change.endpoint_result().error
    );
    let mut recovered = silent::take_sp_endpoint_account(change);
    assert!(
        recovered.is_ok(),
        "SP recovered without an earlier initialized account"
    );
    assert_eq!(
        recovered.name(),
        config.account_name,
        "original SP identity"
    );
    assert_eq!(
        recovered.network(),
        Network::Signet,
        "recovered actual network"
    );
    recovered.close_backend();
    common::cleanup_test_account(&config.account_name);
}

#[test]
fn musig_offline_opening_never_contacts_saved_endpoint_and_failed_opening_can_recover() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("local saved endpoint");
    listener
        .set_nonblocking(true)
        .expect("nonblocking listener");
    let mut config = config(
        true,
        &format!("tcp://{}", listener.local_addr().expect("peer address")),
    );
    let mut account = silent::new_musig_account_offline(&config);
    assert!(
        account.is_ok(),
        "open MuSig offline: {}",
        account.get_error()
    );
    assert_eq!(account.network(), Network::Signet, "actual MuSig network");
    assert_eq!(
        account.descriptor(),
        config.get_descriptor(),
        "actual MuSig descriptor"
    );
    assert_eq!(
        listener
            .accept()
            .expect_err("offline opening must not connect")
            .kind(),
        ErrorKind::WouldBlock
    );
    account.close_backend();
    drop(account);

    let descriptor = config.get_descriptor();
    config.set_descriptor("invalid descriptor".into());
    assert!(
        config.save().is_empty(),
        "persist invalid descriptor fixture"
    );
    let failed = silent::new_musig_account_offline(&config);
    assert!(!failed.is_ok(), "invalid initial offline opening fails");
    config.set_descriptor(descriptor);
    assert!(config.save().is_empty(), "restore valid MuSig config");
    let mut change = silent::musig_endpoint_change(failed, false);
    change.prepare_endpoint();
    assert_eq!(
        change.endpoint_phase(),
        EndpointPhase::Draining,
        "failed opening retained the requested name"
    );
    change.finish_endpoint_after_drain();
    assert!(
        change.endpoint_result().is_ok,
        "fresh command recovers: {}",
        change.endpoint_result().error
    );
    let mut recovered = silent::take_musig_endpoint_account(change);
    assert!(
        recovered.is_ok(),
        "MuSig recovered without an earlier initialized account"
    );
    assert_eq!(
        recovered.name(),
        config.account_name,
        "original MuSig identity"
    );
    assert_eq!(
        recovered.network(),
        Network::Signet,
        "recovered actual network"
    );
    recovered.close_backend();
    common::cleanup_test_account(&config.account_name);
}

#[test]
fn sp_startup_connects_a_single_subaccount_and_preserves_the_first_failure() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("local saved endpoint");
    let endpoint = format!("tcp://{}", listener.local_addr().expect("peer address"));
    drop(listener);
    let config = config(false, &endpoint);
    let account = silent::new_account_offline(&config);
    assert!(account.is_ok(), "initial SP state opens offline");
    let mut startup = silent::sp_startup_endpoint_change(account);
    startup.prepare_endpoint();
    assert_eq!(
        startup.endpoint_phase(),
        EndpointPhase::Draining,
        "startup uses the existing command drain"
    );
    startup.finish_endpoint_after_drain();
    let result = startup.endpoint_result();
    assert!(!result.is_ok, "saved Electrum endpoint is unavailable");
    assert_eq!(
        result.kind,
        BackendErrorKind::Electrum,
        "one sub-account still requests its saved Electrum connection"
    );
    assert_eq!(
        startup.endpoint_server(),
        BackendServer::Electrum,
        "BlindBit failure cannot replace the primary Electrum startup failure"
    );
    let mut account = silent::take_sp_endpoint_account(startup);
    assert!(
        account.is_ok(),
        "failed startup retains a usable offline account"
    );
    assert_eq!(
        account.name(),
        config.account_name,
        "startup keeps its account identity"
    );
    account.close_backend();
    common::cleanup_test_account(&config.account_name);
}
