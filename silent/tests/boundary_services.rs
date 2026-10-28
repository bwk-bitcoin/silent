mod common;

use std::{fs, str::FromStr};

use bitcoin::{
    bip32::{DerivationPath, Xpriv, Xpub},
    secp256k1::Secp256k1,
};
use serde_json::{json, Value};
use silent::{
    commit_key_file, key_file_preview, new_endpoint_retry, new_feed_state, new_keys_module,
    validation_error_kind, BackendErrorKind, KeyFileFailure, Network, PsbtValidation,
};
use temp_dir::TempDir;

fn portable(seed: u8, network: bitcoin::Network) -> Value {
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(network, &[seed; 32]).expect("test master");
    let path = DerivationPath::from_str("m/0'").expect("path");
    let child = master.derive_priv(&secp, &path).expect("child");
    json!({"version":1, "network": network, "fingerprint":master.fingerprint(&secp).to_string(),
        "alias":"Imported", "routes":["sdcard"], "xpubs":[{"path":"m/0'", "xpub":Xpub::from_priv(&secp, &child).to_string()}]})
}

#[test]
fn portable_preview_validates_and_commits_only_the_reviewed_complete_record() {
    common::isolated_state();
    let dir = TempDir::new().expect("portable fixture");
    let path = dir.path().join("key.json");
    for network in [
        bitcoin::Network::Bitcoin,
        bitcoin::Network::Testnet,
        bitcoin::Network::Signet,
        bitcoin::Network::Regtest,
    ] {
        for order in [
            ["usb", "qr", "sdcard"],
            ["usb", "sdcard", "qr"],
            ["qr", "usb", "sdcard"],
            ["qr", "sdcard", "usb"],
            ["sdcard", "usb", "qr"],
            ["sdcard", "qr", "usb"],
        ] {
            for count in 0..=order.len() {
                let mut value = portable(27, network);
                value["routes"] = json!(order[..count]);
                fs::write(&path, value.to_string()).expect("portable file");
                let preview = key_file_preview(path.to_str().expect("path"), network.into());
                assert!(preview.is_ok(), "valid preview: {}", preview.get_error());
                let record = silent::keys::portable::parse(&value.to_string(), network)
                    .expect("portable record");
                let expected = if count == 0 {
                    vec!["sdcard"]
                } else {
                    order[..count].to_vec()
                };
                assert_eq!(
                    record
                        .routes
                        .as_slice()
                        .iter()
                        .map(|route| route.as_str().replace('_', ""))
                        .collect::<Vec<_>>(),
                    expected,
                    "all route orders and empty-route SD fallback"
                );
            }
        }
    }
    let original = portable(27, bitcoin::Network::Regtest);
    for (field, value, kind) in [
        ("version", json!(2), KeyFileFailure::Newer),
        ("network", json!("bitcoin"), KeyFileFailure::Network),
        (
            "fingerprint",
            json!("not-a-fingerprint"),
            KeyFileFailure::Invalid,
        ),
        ("routes", json!(["hot"]), KeyFileFailure::Invalid),
        ("routes", json!(["usb", "usb"]), KeyFileFailure::Invalid),
        ("routes", json!(["unknown"]), KeyFileFailure::Invalid),
        (
            "xpubs",
            json!([{"path":"m/1'", "xpub":original["xpubs"][0]["xpub"]}]),
            KeyFileFailure::Invalid,
        ),
        (
            "xpubs",
            json!([original["xpubs"][0], original["xpubs"][0]]),
            KeyFileFailure::Invalid,
        ),
    ] {
        let mut invalid = original.clone();
        invalid[field] = value;
        fs::write(&path, invalid.to_string()).expect("invalid fixture");
        let preview = key_file_preview(path.to_str().expect("path"), Network::Regtest);
        assert!(!preview.is_ok(), "reject invalid {field}");
        assert_eq!(
            preview.file_failure(),
            kind,
            "typed file failure for {field}"
        );
    }
    fs::write(&path, original.to_string()).expect("original fixture");
    let preview = key_file_preview(path.to_str().expect("path"), Network::Regtest);
    let mut cache = new_keys_module(Network::Regtest);
    fs::write(&path, portable(28, bitcoin::Network::Regtest).to_string())
        .expect("replace file after preview");
    let committed = commit_key_file(&preview, "Chosen alias");
    assert!(
        committed.is_ok(),
        "commit reviewed key: {}",
        committed.get_error()
    );
    assert!(
        cache.apply_key_file(&committed).is_ok,
        "apply committed snapshot without another write"
    );
    assert_eq!(cache.keys().len(), 1, "one complete imported key");
    assert_eq!(
        cache.keys()[0].fingerprint,
        preview.file_fingerprint(),
        "commit uses reviewed data, not changed file"
    );
    assert_eq!(cache.keys()[0].alias, "Chosen alias", "confirmed alias");
    assert!(
        !commit_key_file(&preview, "Duplicate").is_ok(),
        "duplicate fingerprint refused"
    );
    let other = key_file_preview(path.to_str().expect("path"), Network::Regtest);
    assert!(
        !commit_key_file(&other, "Chosen alias").is_ok(),
        "duplicate alias refused at write boundary"
    );
    let root = silent::config::datadir().join("keys");
    let backup = root.with_extension("key-import-backup");
    fs::rename(&root, &backup).expect("retain isolated store");
    fs::write(&root, b"not a directory").expect("block store I/O");
    let failed = commit_key_file(&other, "Second key");
    fs::remove_file(&root).expect("remove test blocker");
    fs::rename(&backup, &root).expect("restore isolated store");
    assert!(!failed.is_ok(), "failed save reports failure");
    assert_eq!(
        failed.get_error_kind(),
        BackendErrorKind::Storage,
        "import preserves the storage failure category"
    );
    assert_eq!(
        new_keys_module(Network::Regtest).keys().len(),
        1,
        "failed import left no partial record"
    );
    fs::write(&path, portable(29, bitcoin::Network::Regtest).to_string())
        .expect("concurrent import fixture");
    let third = key_file_preview(path.to_str().expect("path"), Network::Regtest);
    let first = std::thread::spawn(move || commit_key_file(&other, "Concurrent import"));
    let second = std::thread::spawn(move || commit_key_file(&third, "Concurrent import"));
    let results = [
        first.join().expect("first import"),
        second.join().expect("second import"),
    ];
    assert_eq!(
        results.iter().filter(|result| result.is_ok()).count(),
        1,
        "alias uniqueness is checked under the write lock"
    );
    assert_eq!(
        new_keys_module(Network::Regtest).keys().len(),
        2,
        "the losing concurrent import left no partial key"
    );
}

#[test]
fn feed_state_normalizes_and_invalidates_without_substitution_or_expiry() {
    let mut state = new_feed_state();
    assert_eq!(
        state.fee_selection().preferences.mode,
        0,
        "default fee mode remains Manual"
    );
    assert_eq!(
        state.fee_selection().preferences.source,
        "",
        "default fee selection has no provider"
    );
    state.select_price("configured-but-missing".into(), " eur ".into());
    state.add_feed("other");
    state.set_currencies("other", vec!["EUR".into()]);
    state.set_price_quote("other", "EUR", 42.0);
    assert!(
        !state.price_selection().quote.available,
        "no automatic provider substitution"
    );
    assert_eq!(
        state.price_selection().source,
        "configured-but-missing",
        "missing selection retained"
    );
    state.add_feed("configured-but-missing");
    state.set_currencies(
        "configured-but-missing",
        vec![" eur ".into(), "EUR".into(), "".into(), "usd".into()],
    );
    assert_eq!(
        state.feed_source("configured-but-missing").currencies,
        vec!["EUR", "USD"],
        "canonical offered currencies"
    );
    state.set_price_quote("configured-but-missing", " eur ", 50_000.0);
    for _ in 0..5 {
        assert_eq!(
            state.price_selection().quote.value,
            50_000.0,
            "quotes have no implicit expiry"
        );
    }
    state.set_currencies("configured-but-missing", vec!["USD".into()]);
    assert!(
        !state.price_selection().quote.available,
        "withdrawn currency invalidates its cached quote"
    );
    state.set_currencies("configured-but-missing", vec!["EUR".into()]);
    assert!(
        !state.price_selection().quote.available,
        "re-offering does not resurrect withdrawn quote"
    );
    for invalid in [0.0, -1.0, f64::NAN, f64::INFINITY] {
        state.set_price_quote("configured-but-missing", "EUR", 50_000.0);
        state.set_price_quote("configured-but-missing", "EUR", invalid);
        assert!(
            !state.price_selection().quote.available,
            "invalid price explicitly unavailable"
        );
    }
    let mut fees = state.fee_selection().preferences;
    fees.mode = 2;
    fees.source = "selected".into();
    assert!(state.select_fees(fees.clone()), "valid fee selection");
    state.add_feed("selected");
    state.set_fee_capabilities("selected", true, true);
    state.set_default_fee("selected", 0.1234567);
    state.set_smart_fees("selected", 0.25, f64::NAN, 0.75);
    state.set_fee_capabilities("other", true, true);
    state.set_default_fee("other", 11.0);
    state.set_smart_fees("other", 12.0, 13.0, 14.0);
    let selected = state.fee_selection();
    assert_eq!(
        selected.default_rate.value, 0.1234567,
        "cache preserves provider precision"
    );
    assert!(
        selected.low.available && !selected.medium.available && selected.fast.available,
        "tiers independently unavailable"
    );
    assert_eq!(selected.low.value, 0.25, "selected provider's Low quote");
    assert_eq!(
        selected.medium.value, 0.0,
        "no substitute for unavailable Medium"
    );
    assert_eq!(selected.fast.value, 0.75, "selected provider's Fast quote");
    state.set_fee_capabilities("selected", true, false);
    state.set_fee_capabilities("selected", true, true);
    assert!(
        !state.fee_selection().low.available,
        "capability withdrawal clears cached tiers"
    );
    state.remove_feed("selected");
    assert_eq!(
        state.fee_selection().preferences.source,
        "selected",
        "fee selection survives provider removal"
    );
    assert!(
        !state.fee_selection().default_rate.available,
        "removed provider has no cached value"
    );
    state.set_fee_capabilities("selected", true, true);
    state.set_default_fee("selected", 8.0);
    state.set_smart_fees("selected", 9.0, 10.0, 11.0);
    state.add_feed("selected");
    state.set_fee_capabilities("selected", true, true);
    let reloaded = state.fee_selection();
    assert!(
        !reloaded.default_rate.available
            && !reloaded.low.available
            && !reloaded.medium.available
            && !reloaded.fast.available,
        "removed-source updates and reloading cannot revive cached values"
    );
    fees.mode = 99;
    fees.source = "other".into();
    assert!(!state.select_fees(fees), "invalid fee enum rejected");
    assert_eq!(
        state.fee_selection().preferences.mode,
        2,
        "invalid request does not mutate preferences"
    );
    assert_eq!(
        state.fee_selection().preferences.source,
        "selected",
        "invalid request cannot switch providers"
    );
}

#[test]
fn fee_selection_uses_one_provider_for_every_mode_and_capability_combination() {
    for mode in 0..=2 {
        for has_default in [false, true] {
            for has_smart in [false, true] {
                let mut state = new_feed_state();
                state.add_feed("selected");
                state.set_fee_capabilities("selected", has_default, has_smart);
                state.set_default_fee("selected", 1.25);
                state.set_smart_fees("selected", 2.5, 3.75, 4.125);
                state.add_feed("other");
                state.set_fee_capabilities("other", true, true);
                state.set_default_fee("other", 11.0);
                state.set_smart_fees("other", 12.0, 13.0, 14.0);
                for source in ["selected", "other", "missing", ""] {
                    let mut preferences = state.fee_selection().preferences;
                    preferences.mode = mode;
                    preferences.source = source.into();
                    assert!(
                        state.select_fees(preferences),
                        "select {source:?} in mode {mode}"
                    );
                    let selected = state.fee_selection();
                    assert_eq!(selected.preferences.mode, mode, "selection retains mode");
                    assert_eq!(
                        selected.preferences.source, source,
                        "selection retains identity"
                    );
                    let expected = match source {
                        "selected" => [
                            (has_default, if has_default { 1.25 } else { 0.0 }),
                            (has_smart, if has_smart { 2.5 } else { 0.0 }),
                            (has_smart, if has_smart { 3.75 } else { 0.0 }),
                            (has_smart, if has_smart { 4.125 } else { 0.0 }),
                        ],
                        "other" => [(true, 11.0), (true, 12.0), (true, 13.0), (true, 14.0)],
                        _ => [(false, 0.0); 4],
                    };
                    for (quote, expected) in [
                        selected.default_rate,
                        selected.low,
                        selected.medium,
                        selected.fast,
                    ]
                    .into_iter()
                    .zip(expected)
                    {
                        assert_eq!(
                            (quote.available, quote.value), expected,
                            "source {source:?}, mode {mode}, default {has_default}, smart {has_smart}"
                        );
                    }
                }
                state.set_fee_capabilities("selected", false, false);
                state.set_default_fee("selected", 21.0);
                state.set_smart_fees("selected", 22.0, 23.0, 24.0);
                state.set_fee_capabilities("selected", true, true);
                let mut preferences = state.fee_selection().preferences;
                preferences.source = "selected".into();
                assert!(
                    state.select_fees(preferences),
                    "reselect withdrawn provider"
                );
                let selected = state.fee_selection();
                assert!(
                    !selected.default_rate.available
                        && !selected.low.available
                        && !selected.medium.available
                        && !selected.fast.available,
                    "withdrawal clears every cached value and ignores unsupported updates"
                );
                state.set_default_fee("selected", 5.25);
                state.set_smart_fees("selected", 6.5, 7.75, 8.125);
                let updated = state.fee_selection();
                assert!(
                    updated.default_rate.available
                        && updated.low.available
                        && updated.medium.available
                        && updated.fast.available,
                    "fresh updates restore every recommendation"
                );
                assert_eq!(
                    [
                        updated.default_rate.value,
                        updated.low.value,
                        updated.medium.value,
                        updated.fast.value,
                    ],
                    [5.25, 6.5, 7.75, 8.125],
                    "push updates all come from the selected provider"
                );
            }
        }
    }
}

#[test]
fn fee_selection_preserves_every_partial_unavailability_combination() {
    for mode in 0..=2 {
        for mask in 0..16 {
            for invalid in [0.0, -1.0, f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
                let mut state = new_feed_state();
                state.add_feed("selected");
                state.set_fee_capabilities("selected", true, true);
                state.add_feed("other");
                state.set_fee_capabilities("other", true, true);
                state.set_default_fee("other", 11.0);
                state.set_smart_fees("other", 12.0, 13.0, 14.0);
                let mut preferences = state.fee_selection().preferences;
                preferences.mode = mode;
                preferences.source = "selected".into();
                assert!(state.select_fees(preferences), "select partial provider");
                let values = [1.25, 2.5, 3.75, 4.125];
                let offered: [f64; 4] = std::array::from_fn(|index| {
                    if mask & (1 << index) != 0 {
                        values[index]
                    } else {
                        invalid
                    }
                });
                state.set_default_fee("selected", values[0]);
                state.set_smart_fees("selected", values[1], values[2], values[3]);
                state.set_default_fee("selected", offered[0]);
                state.set_smart_fees("selected", offered[1], offered[2], offered[3]);
                let selected = state.fee_selection();
                for (index, quote) in [
                    selected.default_rate,
                    selected.low,
                    selected.medium,
                    selected.fast,
                ]
                .into_iter()
                .enumerate()
                {
                    let available = mask & (1 << index) != 0;
                    assert_eq!(
                        (quote.available, quote.value),
                        (available, if available { values[index] } else { 0.0 }),
                        "mode {mode}, availability mask {mask}, invalid {invalid}, quote {index}"
                    );
                }
            }
        }
    }
}

#[test]
fn retry_budget_uses_explicit_time_and_ignores_duplicate_loss_notifications() {
    let mut retry = new_endpoint_retry();
    assert!(
        !retry.retry_lost(0).schedule,
        "unarmed connection does not retry"
    );
    retry.retry_connected();
    for (time, count) in [(0, 1), (2000, 2), (4000, 3)] {
        let decision = retry.retry_lost(time);
        assert!(decision.schedule && !decision.give_up, "retry loss {count}");
        assert_eq!(decision.losses, count, "one loss per attempt");
        assert_eq!(decision.delay_ms, 2000, "retry delay");
        assert!(
            !retry.retry_lost(time + 1).schedule,
            "duplicate notification ignored"
        );
        assert!(retry.retry_due(), "timer asks for one new attempt");
    }
    let decision = retry.retry_lost(6000);
    assert!(
        decision.give_up && !decision.schedule,
        "fourth loss exhausts budget"
    );
    assert_eq!(decision.losses, 4, "reported exhausted budget");
    assert!(!retry.retry_due(), "exhausted connection remains down");
    retry.retry_connected();
    assert!(
        retry.retry_lost(70_000).schedule,
        "old losses are outside the window"
    );
    retry.retry_stopped();
    assert!(
        !retry.retry_due() && !retry.retry_lost(70_001).schedule,
        "intentional disconnect cancels retry"
    );
}

#[test]
fn error_categories_do_not_depend_on_english_diagnostics() {
    let connection = silent::test_electrum("wss://spent-reuse-sign-network:1".into(), true);
    assert_eq!(
        connection.kind,
        BackendErrorKind::InvalidInput,
        "parse failure category is carried from the source, not inferred from words"
    );
    assert!(
        connection.error.contains("spent-reuse-sign-network"),
        "diagnostic retains the untrusted address"
    );
    let info = silent::get_backend_info("http://127.0.0.1:1".into());
    assert_eq!(
        info.kind,
        BackendErrorKind::Network,
        "upstream typed network failure survives the bridge"
    );
    let mut validation = PsbtValidation {
        is_ok: true,
        error: String::new(),
        is_valid: false,
        issues: "arbitrary localized text".into(),
        reused_output_count: 0,
        spent_input_count: 1,
    };
    assert_eq!(
        validation_error_kind(&validation),
        BackendErrorKind::InputConflict,
        "numeric spent evidence"
    );
    validation.spent_input_count = 0;
    validation.reused_output_count = 1;
    assert_eq!(
        validation_error_kind(&validation),
        BackendErrorKind::AddressReuse,
        "numeric reuse evidence"
    );
}

#[test]
fn endpoint_command_owns_test_save_close_drain_and_reopen_order() {
    common::isolated_state();
    let name = common::test_account_name();
    let config = silent::new_config(
        name.clone(),
        Network::Regtest,
        common::descriptor_from_mnemonic(common::TEST_MNEMONIC, Network::Regtest),
        "http://127.0.0.1:1".into(),
        String::new(),
        546,
        "sp".into(),
    );
    assert!(config.save().is_empty(), "save isolated SP account");
    let mut account = silent::new_account(name.clone());
    assert!(
        account.is_ok(),
        "offline SP account: {}",
        account.get_error()
    );
    let receiver = account.take_receiver();
    let mut failed = silent::sp_endpoint_change(
        account,
        silent::BackendServer::Electrum,
        "unsupported://127.0.0.1:1".into(),
        true,
        true,
        false,
    );
    failed.prepare_endpoint();
    assert_eq!(
        failed.endpoint_phase(),
        silent::EndpointPhase::Complete,
        "failed test stops before shutdown"
    );
    assert!(
        !failed.endpoint_result().is_ok && !failed.endpoint_result().saved,
        "failed test is neither saved nor connected"
    );
    let account = silent::take_sp_endpoint_account(failed);
    assert!(
        account.is_ok(),
        "failed test preserved live account ownership"
    );
    let mut change = silent::sp_endpoint_change(
        account,
        silent::BackendServer::Electrum,
        String::new(),
        true,
        false,
        false,
    );
    change.prepare_endpoint();
    assert_eq!(
        change.endpoint_phase(),
        silent::EndpointPhase::Draining,
        "reopen is fenced behind receiver drain"
    );
    assert!(
        !change.endpoint_result().is_ok,
        "dispatch is not connection completion"
    );
    while receiver.recv().is_some() {}
    change.finish_endpoint_after_drain();
    assert_eq!(
        change.endpoint_phase(),
        silent::EndpointPhase::Complete,
        "completed command"
    );
    assert!(
        change.endpoint_result().is_ok,
        "intentional offline reopen succeeds"
    );
    change.finish_endpoint_after_drain();
    assert!(
        !change.endpoint_result().is_ok,
        "duplicate completion is explicitly refused"
    );
    let account = silent::take_sp_endpoint_account(change);
    assert!(
        account.is_ok(),
        "duplicate completion did not destroy account"
    );
    let mut missing_config = silent::sp_endpoint_change(
        account,
        silent::BackendServer::Electrum,
        String::new(),
        true,
        false,
        false,
    );
    missing_config.prepare_endpoint();
    common::logical::rename(&config, "config.json", "endpoint-backup.json");
    missing_config.finish_endpoint_after_drain();
    assert!(
        !missing_config.endpoint_result().is_ok,
        "failed reopen is not successful connection completion"
    );
    assert_eq!(
        missing_config.endpoint_result().kind,
        BackendErrorKind::Storage,
        "config read failure retains its source category"
    );
    common::logical::rename(&config, "endpoint-backup.json", "config.json");
    let account = silent::take_sp_endpoint_account(missing_config);
    assert!(!account.is_ok(), "failed reopen leaves a closed account");
    let mut retry = silent::sp_endpoint_change(
        account,
        silent::BackendServer::Electrum,
        String::new(),
        true,
        false,
        false,
    );
    retry.prepare_endpoint();
    assert_eq!(
        retry.endpoint_phase(),
        silent::EndpointPhase::Draining,
        "fresh command accepts the retained recovery identity"
    );
    retry.finish_endpoint_after_drain();
    assert!(
        retry.endpoint_result().is_ok,
        "public command recovers after the config becomes readable again: {}",
        retry.endpoint_result().error
    );
    let account = silent::take_sp_endpoint_account(retry);
    assert!(
        account.is_ok() && account.get_error().is_empty(),
        "recovered account is ready without a stale initialization error"
    );
    assert_eq!(account.name(), name, "SP recovery retains account identity");
    drop(account);
    common::cleanup_test_account(&name);
}
