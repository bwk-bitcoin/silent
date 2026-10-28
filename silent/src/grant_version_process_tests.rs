use crate::{
    app_state::{
        app_plugin_access_request, app_plugin_access_revision, app_set_plugin_access,
        app_set_plugin_access_if_current, AppState, StoredGrant,
    },
    ffi::{PluginAccess, PluginGrant},
    removal_process_tests::{plugin_hash, run_scenario, Worker, WorkerCommand, PLUGIN},
};

#[test]
fn prompt_save_refuses_another_process_account_denial() {
    run_scenario("grant-version-denial");
}

#[test]
fn prompt_save_refuses_a_missing_record_changed_in_another_process() {
    run_scenario("grant-version-missing");
}

#[test]
fn prompt_save_refuses_another_process_aba() {
    run_scenario("grant-version-aba");
}

pub(crate) fn run(case: &str) {
    let initial = app_plugin_access_request(PLUGIN.into(), plugin_hash());
    assert!(initial.is_ok, "initial snapshot: {}", initial.error);
    let account = if case == "missing" {
        PluginGrant::Unasked
    } else {
        PluginGrant::Allowed
    };
    assert_eq!(
        app_set_plugin_access(
            PluginAccess {
                account,
                ..initial.access
            },
            initial.storage_epoch,
        ),
        "",
        "process A establishes the initial answers"
    );
    let snapshot = app_plugin_access_request(PLUGIN.into(), plugin_hash());
    assert!(snapshot.is_ok, "process A snapshot: {}", snapshot.error);
    assert_eq!(snapshot.access.account, account, "initial Account answer");
    assert_eq!(
        snapshot.access.keys,
        PluginGrant::Unasked,
        "Keys not asked yet"
    );
    let mut worker = Worker::spawn();
    assert_eq!(
        worker.call(WorkerCommand::GrantAccount {
            grant: StoredGrant::Denied,
        }),
        serde_json::json!({"error": ""}),
        "process B publishes Account Denied"
    );
    let expected_account = match case {
        "denial" => PluginGrant::Denied,
        "missing" | "aba" => {
            let restored = if case == "missing" {
                StoredGrant::Unasked
            } else {
                StoredGrant::Allowed
            };
            assert_eq!(
                worker.call(WorkerCommand::GrantAccount { grant: restored }),
                serde_json::json!({"error": ""}),
                "process B restores the original answers, including an absent record"
            );
            account
        }
        _ => panic!("unknown grant version case {case}"),
    };
    worker.finish();
    assert_eq!(
        app_plugin_access_revision(),
        snapshot.grant_revision,
        "process B cannot change process A's in-memory revision"
    );
    let current = app_plugin_access_request(PLUGIN.into(), plugin_hash());
    assert!(
        current.is_ok,
        "read process B publication: {}",
        current.error
    );
    assert_eq!(
        current.storage_epoch, snapshot.storage_epoch,
        "no removal occurred"
    );
    assert_eq!(
        current.grant_version,
        snapshot.grant_version + if case == "denial" { 1 } else { 2 },
        "the persisted version records every process B mutation even after ABA"
    );
    if case == "missing" {
        assert!(
            AppState::try_load()
                .expect("state with no grant records")
                .plugin_access
                .is_empty(),
            "the version is retained even after process B drops the last record"
        );
    }
    assert_eq!(
        app_set_plugin_access_if_current(
            PluginAccess {
                keys: PluginGrant::Allowed,
                ..snapshot.access
            },
            snapshot.storage_epoch,
            snapshot.grant_version,
        ),
        "Access answers changed since they were read: these conflicting answers were not saved",
        "process A's stale Keys Allow is an explicit conflict"
    );
    let after = app_plugin_access_request(PLUGIN.into(), plugin_hash());
    assert!(after.is_ok, "read refused save: {}", after.error);
    assert_eq!(
        after.access.account, expected_account,
        "process B's Account answer survives"
    );
    assert_eq!(
        after.access.keys,
        PluginGrant::Unasked,
        "stale Keys Allow never publishes"
    );
    assert_eq!(
        after.access.state,
        PluginGrant::Unasked,
        "State remains unasked"
    );
    assert_eq!(
        after.grant_version, current.grant_version,
        "a conflict is not a mutation"
    );
}
