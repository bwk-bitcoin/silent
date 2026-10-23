//! Production-key and serialized-session checks, without the unit-test unlock fallback.

use std::fs;

use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use silent::{
    app_state::{app_plugin_access_request, app_set_plugin_access, StoredGrant},
    set_datadir, state_crypt,
    storage::{storage_request, storage_session_open, storage_session_revoke},
    PluginAccess,
};
use temp_dir::TempDir;

const XPUB: &str = "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa";

fn request(session: u64, mut message: Value) -> Value {
    message["id"] = json!("contract");
    let response: Value = serde_json::from_str(&storage_request(session, message.to_string()))
        .expect("storage returns a JSON response");
    assert_eq!(response["id"], "contract", "request identity roundtrips");
    response
}

fn open(session: u64) -> String {
    let response = request(session, json!({"op":"open", "storage":"private"}));
    assert_eq!(response["error"], "", "authorized root opens: {response}");
    response["storage"]
        .as_str()
        .expect("opaque serialized handle")
        .to_owned()
}

fn grant(hash: &str, state: StoredGrant) {
    let request = app_plugin_access_request("contract.plugin".into(), hash.into());
    assert!(request.is_ok, "grant request: {}", request.error);
    let saved = app_set_plugin_access(
        PluginAccess {
            state: state.into(),
            ..request.access
        },
        request.storage_epoch,
    );
    assert_eq!(saved, "", "grant saved");
}

#[test]
fn production_sessions_require_unlock_and_current_hash_grants() {
    let temp = TempDir::new().expect("isolated storage directory");
    set_datadir(temp.path().to_path_buf());
    let old = storage_session_open("contract.plugin".into(), "old-hash".into(), true);
    let locked = request(old, json!({"op":"open", "storage":"private"}));
    assert_eq!(
        locked["error"], "locked",
        "no test-key fallback in production"
    );

    state_crypt::create_with_device(42, XPUB).expect("create device-key state without a device");
    assert_eq!(
        request(old, json!({"op":"open", "storage":"private"}))["error"],
        "denied",
        "unasked Saved Data is denied"
    );
    grant("old-hash", StoredGrant::Allowed);
    let handle = open(old);
    let payload = b"\0opaque plugin payload\xff";
    let written = request(
        old,
        json!({"op":"write", "storage":handle, "path":"persist.bin",
               "mode":"create", "data":STANDARD.encode(payload)}),
    );
    assert_eq!(written["error"], "", "complete file commits: {written}");

    let plugin_root = temp.path().join("contract.plugin");
    assert!(
        plugin_root.is_dir(),
        "only plugin identity is visible as the root"
    );
    let mut envelopes = 0;
    for entry in fs::read_dir(&plugin_root).expect("inspect host-side backing files") {
        let entry = entry.expect("backing entry");
        let name = entry.file_name().to_string_lossy().to_string();
        assert!(
            !name.contains("persist"),
            "logical name is not a physical name"
        );
        let bytes = fs::read(entry.path()).expect("read host-side encrypted backing");
        if bytes.is_empty() {
            continue;
        }
        assert!(
            state_crypt::is_sealed(&bytes),
            "all nonempty backing files are BIP138"
        );
        assert!(
            !bytes.windows(payload.len()).any(|window| window == payload),
            "plugin payload is concealed"
        );
        let metadata = bip138::EncryptedMetadata::from_encrypted_payload(&bytes)
            .expect("preserved BIP138 envelope metadata");
        assert_eq!(
            metadata.derivation_paths,
            vec![state_crypt::device_path(42).expect("known device derivation")],
            "each backing envelope retains its device derivation"
        );
        envelopes += 1;
    }
    assert!(envelopes > 0, "a complete encrypted tree was persisted");

    let upgraded = storage_session_open("contract.plugin".into(), "new-hash".into(), true);
    assert_eq!(
        request(upgraded, json!({"op":"open", "storage":"private"}))["error"],
        "denied",
        "new library hash does not inherit Saved Data"
    );
    grant("new-hash", StoredGrant::Allowed);
    let upgraded_handle = open(upgraded);
    assert_eq!(
        request(
            upgraded,
            json!({"op":"read", "storage":handle, "path":"persist.bin"})
        )["error"],
        "invalid_handle",
        "even the same plugin cannot use another connection's handle"
    );
    let read = request(
        upgraded,
        json!({"op":"read", "storage":upgraded_handle, "path":"persist.bin"}),
    );
    assert_eq!(
        read["data"],
        STANDARD.encode(payload),
        "upgrade reopens stable plugin data"
    );

    grant("old-hash", StoredGrant::Denied);
    assert_eq!(
        request(
            old,
            json!({"op":"read", "storage":handle, "path":"persist.bin"})
        )["error"],
        "denied",
        "revocation affects an existing open handle"
    );
    state_crypt::lock();
    assert_eq!(
        request(
            upgraded,
            json!({"op":"read", "storage":upgraded_handle, "path":"persist.bin"})
        )["error"],
        "locked",
        "locking refuses an already opened root"
    );
    state_crypt::unlock_with_device(XPUB).expect("unlock with the same public test vector");
    storage_session_revoke(upgraded);
    assert_eq!(
        request(
            upgraded,
            json!({"op":"read", "storage":upgraded_handle, "path":"persist.bin"})
        )["error"],
        "revoked",
        "disconnect permanently revokes the connection"
    );
    let restarted = storage_session_open("contract.plugin".into(), "new-hash".into(), true);
    let restarted_handle = open(restarted);
    assert_eq!(
        request(
            restarted,
            json!({"op":"read", "storage":restarted_handle, "path":"persist.bin"})
        )["data"],
        STANDARD.encode(payload),
        "fresh session reloads committed files after unlock"
    );
    storage_session_revoke(old);
    storage_session_revoke(restarted);
    state_crypt::lock();
}
