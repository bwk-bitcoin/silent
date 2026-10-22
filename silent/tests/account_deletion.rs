mod common;

use std::{process::Command, str::FromStr};

use bitcoin::{
    bip32::{DerivationPath, Xpriv, Xpub},
    secp256k1::Secp256k1,
};
use silent::{
    config::Config, delete_musig_account, delete_sp_account, musig_account_delete_check,
    new_account_offline, sp_account_delete_check, state_crypt, Network,
};

fn account(multikey: bool) -> Config {
    let name = common::test_account_name();
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(bitcoin::Network::Regtest, &[41; 32]).expect("fixture master");
    let child = master
        .derive_priv(
            &secp,
            &DerivationPath::from_str("m/86'/1'/0'").expect("fixture path"),
        )
        .expect("fixture role");
    let descriptor = format!(
        "tr([{}/86'/1'/0']{}/<0;1>/*)",
        master.fingerprint(&secp),
        Xpub::from_priv(&secp, &child)
    );
    let mut config = Config::new(
        name,
        Network::Regtest,
        if multikey {
            descriptor.clone()
        } else {
            common::descriptor_from_mnemonic(common::TEST_MNEMONIC, Network::Regtest)
        },
        "http://127.0.0.1:1".into(),
        String::new(),
        None,
    );
    if multikey {
        config.plugin_id = "musig".into();
    } else {
        config.taproot_descriptor = descriptor;
    }
    assert!(config.save().is_empty(), "create isolated logical account");
    config
}

#[cfg(unix)]
#[test]
fn closed_accounts_report_locked_and_io_failures_without_removal() {
    if common::logical::isolated_process(
        "closed_accounts_report_locked_and_io_failures_without_removal",
    ) {
        return;
    }
    let sp = account(false);
    let musig = account(true);
    let duplicate = silent::create_musig_account(silent::MusigCreationRequest {
        account_name: sp.account_name.clone(),
        network: Network::Regtest,
        descriptor: musig.descriptor.clone(),
        electrum_url: String::new(),
        verify_certificate: true,
    });
    assert!(
        !duplicate.is_ok && duplicate.error == "already_exists",
        "creation preserves globally unique built-in account IDs"
    );
    let mut backend = new_account_offline(&sp);
    assert!(backend.is_ok(), "real SP account: {}", backend.get_error());
    assert_eq!(
        delete_sp_account(sp.account_name.clone()),
        "busy",
        "live backend owns the storage lease"
    );
    assert!(
        backend.shutdown_backend().is_empty(),
        "real shutdown barrier"
    );
    drop(backend);
    struct DeletionCase<'a> {
        config: &'a Config,
        check: fn(String) -> String,
        delete: fn(String) -> String,
    }
    let cases = [
        DeletionCase {
            config: &sp,
            check: sp_account_delete_check,
            delete: delete_sp_account,
        },
        DeletionCase {
            config: &musig,
            check: musig_account_delete_check,
            delete: delete_musig_account,
        },
    ];
    for DeletionCase {
        config,
        check,
        delete,
    } in cases
    {
        let previous = common::logical::read(config, "config.json");
        state_crypt::lock();
        assert_eq!(
            check(config.account_name.clone()),
            "locked",
            "locked identity check"
        );
        assert_eq!(
            delete(config.account_name.clone()),
            "locked",
            "locked removal fails explicitly"
        );
        state_crypt::unlock_for_tests();
        let read_only = common::logical::ReadOnlyStorage::new(config);
        assert_eq!(
            delete(config.account_name.clone()),
            "io",
            "real publication/lease I/O failure"
        );
        drop(read_only);
        assert_eq!(
            common::logical::read(config, "config.json"),
            previous,
            "failed deletion retains exact serialized config"
        );
        assert!(
            Config::from_file(config.account_name.clone()).is_ok(),
            "failed deletion remains discoverable"
        );
    }
    assert!(
        !delete_musig_account(sp.account_name.clone()).is_empty(),
        "wrong module cannot remove an account"
    );
    assert!(
        delete_sp_account(sp.account_name.clone()).is_empty(),
        "closed SP account deletes after storage recovers"
    );
    assert!(
        Config::from_file(musig.account_name.clone()).is_ok(),
        "other module preserved"
    );
    assert!(
        delete_musig_account(musig.account_name.clone()).is_empty(),
        "closed MuSig account deletes"
    );
    assert!(
        Config::from_file(sp.account_name).is_err()
            && Config::from_file(musig.account_name).is_err(),
        "both authorized subtrees are absent"
    );
}

#[cfg(unix)]
#[test]
fn endpoint_teardown_cannot_forget_a_failed_flush_before_deletion() {
    if common::logical::isolated_process(
        "endpoint_teardown_cannot_forget_a_failed_flush_before_deletion",
    ) {
        return;
    }
    macro_rules! failed_close {
        ($config:expr, $open:path) => {{
            let mut backend = $open(&$config);
            assert!(
                backend.is_ok(),
                "real backend before failed endpoint teardown: {}",
                backend.get_error()
            );
            let read_only = common::logical::ReadOnlyStorage::new(&$config);
            backend.close_backend();
            drop(read_only);
            let result = backend.reopen_backend(false);
            assert!(
                !result.is_ok && result.kind == silent::BackendErrorKind::Storage,
                "a failed flush cannot silently reopen stale state: {}",
                result.error
            );
            assert!(
                !backend.shutdown_backend().is_empty(),
                "later deletion shutdown still sees the earlier persistence failure"
            );
            assert!(
                Config::from_file($config.account_name.clone()).is_ok(),
                "failed teardown preserved discovery data"
            );
            drop(backend);
        }};
    }
    let sp = account(false);
    failed_close!(sp, silent::new_account_offline);
    let musig = account(true);
    failed_close!(musig, silent::new_musig_account_offline);
}

#[test]
fn account_lease_child() {
    let Ok(directory) = std::env::var("SILENT_ACCOUNT_LEASE_DIRECTORY") else {
        return;
    };
    silent::set_datadir(directory.into());
    state_crypt::unlock_for_tests();
    let name = std::env::var("SILENT_ACCOUNT_LEASE_NAME").expect("child account ID");
    let config = Config::from_file(name).expect("shared encrypted logical config");
    let mut account = new_account_offline(&config);
    if std::env::var("SILENT_ACCOUNT_LEASE_EXPECT_BUSY").as_deref() == Ok("yes") {
        assert!(
            !account.is_ok() && account.get_error().contains("busy"),
            "another process is still using the account: {}",
            account.get_error()
        );
        assert_eq!(
            delete_sp_account(config.account_name),
            "busy",
            "another process cannot remove a live account"
        );
    } else {
        assert!(
            account.is_ok(),
            "process lease released only after backend destruction: {}",
            account.get_error()
        );
        assert!(
            account.shutdown_backend().is_empty(),
            "child drains real backend"
        );
    }
}

#[test]
fn ownership_survives_another_process_and_releases_after_shutdown() {
    let config = account(false);
    let mut account = new_account_offline(&config);
    assert!(account.is_ok(), "parent backend: {}", account.get_error());
    let run = |busy: bool| {
        let status = Command::new(std::env::current_exe().expect("test executable"))
            .args(["--exact", "account_lease_child", "--test-threads=1"])
            .env("SILENT_ACCOUNT_LEASE_DIRECTORY", &config.data_dir)
            .env("SILENT_ACCOUNT_LEASE_NAME", &config.account_name)
            .env(
                "SILENT_ACCOUNT_LEASE_EXPECT_BUSY",
                if busy { "yes" } else { "no" },
            )
            .status()
            .expect("independent process");
        assert!(
            status.success(),
            "cross-process lease, busy={busy}: {status}"
        );
    };
    run(true);
    assert!(
        account.shutdown_backend().is_empty(),
        "all backends drain before ownership release"
    );
    drop(account);
    run(false);
    assert!(
        delete_sp_account(config.account_name).is_empty(),
        "closed account removes through logical storage"
    );
}
