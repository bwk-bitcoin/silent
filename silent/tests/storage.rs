//! Plugin file storage through the host bridge (ws-plaude/silent#60): the Saved Data
//! grant of the library hash decides, a locked state refuses, and an upgraded library
//! keeps its plugin's tree.

use serde_json::{json, Value};
use silent::{
    app_plugin_access_request, app_set_plugin_access, set_datadir,
    state_crypt::{self, unlock_for_tests},
    storage_delivery_error, storage_request, storage_session_generation, storage_session_open,
    storage_session_revoke, PluginAccess, PluginGrant,
};
use temp_dir::TempDir;

const PLUGIN: &str = "example";
const HASH_V1: &str = "AA11";
const HASH_V2: &str = "bb22";

fn send(session: u64, request: Value) -> Value {
    serde_json::from_str(&storage_request(session, request.to_string())).expect("a JSON response")
}

fn ok() -> Value {
    json!({"id": "r", "error": ""})
}

fn failed(code: &str) -> Value {
    json!({"id": "r", "error": code})
}

fn grant_state(hash: &str, state: PluginGrant) {
    let request = app_plugin_access_request(PLUGIN.into(), hash.into());
    assert!(request.is_ok, "grant request: {}", request.error);
    let saved = app_set_plugin_access(
        PluginAccess {
            state,
            ..request.access
        },
        request.storage_epoch,
    );
    assert_eq!(saved, "", "grant saved");
}

fn open(session: u64) -> String {
    let opened = send(
        session,
        json!({"id": "r", "op": "open", "storage": "private"}),
    );
    assert_eq!(opened["error"], "", "the private storage opens: {opened}");
    opened["storage"]
        .as_str()
        .expect("a storage handle")
        .to_string()
}

fn read(session: u64, handle: &str, path: &str) -> Value {
    send(
        session,
        json!({"id": "r", "op": "read", "storage": handle, "path": path}),
    )
}

// One test: the data directory and the unlock key are process wide.
#[test]
fn grants_lock_and_upgrades_decide_who_reaches_the_tree() {
    let tmp = TempDir::new().expect("create temp dir");
    set_datadir(tmp.path().to_path_buf());
    unlock_for_tests();

    let v1 = storage_session_open(PLUGIN.into(), HASH_V1.into(), true);
    assert_ne!(v1, 0, "an external session opens");
    for request in [
        json!({"id": "r", "op": "discover"}),
        json!({"id": "r", "op": "open", "storage": "private"}),
    ] {
        assert_eq!(
            send(v1, request.clone()),
            failed("denied"),
            "unasked is denied: {request}"
        );
    }
    grant_state(HASH_V1, PluginGrant::Allowed);
    assert_eq!(
        send(v1, json!({"id": "r", "op": "discover"})),
        json!({"id": "r", "error": "", "storages": [{"id": "private", "name": "Private plugin storage"}]})
    );
    let h1 = open(v1);
    assert_eq!(
        send(
            v1,
            json!({"id": "r", "op": "mkdir", "storage": h1, "path": "notes"})
        ),
        ok()
    );
    assert_eq!(
        send(
            v1,
            json!({"id": "r", "op": "write", "storage": h1, "path": "notes/a.txt", "data": "djE=", "mode": "create"})
        ),
        ok()
    );

    // The upgraded library has its own grant, and the same tree.
    let v2 = storage_session_open(PLUGIN.into(), HASH_V2.into(), true);
    assert_eq!(
        send(v2, json!({"id": "r", "op": "discover"})),
        failed("denied"),
        "grants are per library hash"
    );
    grant_state(HASH_V2, PluginGrant::Allowed);
    let h2 = open(v2);
    assert_eq!(
        read(v2, &h2, "notes/a.txt"),
        json!({"id": "r", "error": "", "data": "djE="})
    );

    // A grant withdrawn applies to the next request of an open session, and fences the
    // answers computed before it.
    let v1_sent = storage_session_generation(v1);
    let v2_sent = storage_session_generation(v2);
    assert_eq!(storage_delivery_error(v1, v1_sent), "");
    grant_state(HASH_V1, PluginGrant::Denied);
    assert_eq!(storage_delivery_error(v1, v1_sent), "denied");
    assert_eq!(
        storage_delivery_error(v2, v2_sent),
        "",
        "another library's grant"
    );
    assert_eq!(read(v1, &h1, "notes/a.txt"), failed("denied"));
    assert_eq!(
        read(v2, &h2, "notes/a.txt"),
        json!({"id": "r", "error": "", "data": "djE="})
    );

    // Locked, every session is refused, the host's own included.
    let builtin = storage_session_open(PLUGIN.into(), String::new(), false);
    let hb = open(builtin);
    let builtin_sent = storage_session_generation(builtin);
    state_crypt::lock();
    assert_eq!(storage_delivery_error(builtin, builtin_sent), "locked");
    assert_eq!(read(v2, &h2, "notes/a.txt"), failed("locked"));
    assert_eq!(
        send(v2, json!({"id": "r", "op": "discover"})),
        failed("locked")
    );
    assert_eq!(read(builtin, &hb, "notes/a.txt"), failed("locked"));
    assert_eq!(
        send(
            builtin,
            json!({"id": "r", "op": "migrate", "storage": hb, "path": "state.json"})
        ),
        failed("locked")
    );
    unlock_for_tests();
    assert_eq!(
        storage_delivery_error(builtin, builtin_sent),
        "denied",
        "computed under the key held before"
    );
    assert_eq!(
        storage_delivery_error(builtin, storage_session_generation(builtin)),
        ""
    );
    assert_eq!(
        read(builtin, &hb, "notes/a.txt"),
        json!({"id": "r", "error": "", "data": "djE="})
    );

    storage_session_revoke(v2);
    assert_eq!(read(v2, &h2, "notes/a.txt"), failed("revoked"));
    assert_eq!(storage_delivery_error(v2, v2_sent), "revoked");
    assert_eq!(read(builtin, &h2, "notes/a.txt"), failed("invalid_handle"));

    let mut roots: Vec<String> = std::fs::read_dir(tmp.path().join(PLUGIN))
        .expect("the plugin root")
        .map(|entry| {
            entry
                .expect("entry")
                .file_name()
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    roots.sort();
    assert_eq!(
        roots,
        vec!["tree.bip138".to_string(), "tree.lock".to_string()],
        "one tree and its persistent lock for every library version"
    );
    storage_session_revoke(v1);
    storage_session_revoke(builtin);
}
